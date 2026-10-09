#pragma once

// ProtocolTranslator189 -- 1.2.5 (protocol 29, pre-Netty) <-> 1.8.9
// (protocol 47, Netty framing) translation core.
//
// Pure byte-level logic with no engine dependencies (no Packet, no zlib,
// no iostream framing): every function here works on byte vectors, so the
// CTest target in tests/ can exercise it without the game. Wire I/O,
// zlib and the NetworkManager hooks live in ProtocolTranslator189IO.
//
// Direction naming follows the proxy this was ported from (proxy/*.py):
// "serverbound" = game client -> 1.8.9 server, "clientbound" = 1.8.9
// server -> game client. MVP scope matches the proxy: ping/login (offline
// 1.8.9 servers), keepalive, chat, movement, chunks, block changes,
// health, respawn, tab list, digging/placing, held slot, use entity,
// animations. Entities, inventories and online-mode encryption are
// phase 2 (1.8 frames for them are consumed and dropped).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace translator189
{

constexpr int kNativeProtocol = 29;
constexpr int kTargetProtocol = 47;

// 1.8.9 play packet ids we translate (S->C).
enum ServerPlay : uint32_t
{
	kKeepAlive = 0x00,
	kJoinGame = 0x01,
	kChat = 0x02,
	kTimeUpdate = 0x03,
	kSpawnPosition = 0x05,
	kUpdateHealth = 0x06,
	kRespawn = 0x07,
	kPlayerPosLook = 0x08,
	kAnimation = 0x0B,
	kChunkData = 0x21,
	kMultiBlockChange = 0x22,
	kBlockChange = 0x23,
	kPlayerList = 0x38,
	kPluginMessage = 0x3F,
	kDisconnect = 0x40,
};

// 1.8.9 login packet ids (S->C).
enum ServerLogin : uint32_t
{
	kLoginDisconnect = 0x00,
	kEncryptionRequest = 0x01,
	kLoginSuccess = 0x02,
	kSetCompression = 0x03,
};

// Native 1.2.5 packet ids we synthesize or consume.
enum Native125 : uint8_t
{
	kNKeepAlive = 0x00,
	kNLogin = 0x01,
	kNHandshake = 0x02,
	kNChat = 0x03,
	kNTime = 0x04,
	kNSpawnPos = 0x06,
	kNUseEntity = 0x07,
	kNHealth = 0x08,
	kNRespawn = 0x09,
	kNFlying = 0x0A,
	kNPos = 0x0B,
	kNLook = 0x0C,
	kNPosLook = 0x0D,
	kNDig = 0x0E,
	kNPlace = 0x0F,
	kNHeld = 0x10,
	kNAnimation = 0x12,
	kNEntityAction = 0x13,
	kNPreChunk = 0x32,
	kNMapChunk = 0x33,
	kNMultiBlock = 0x34,
	kNBlockChange = 0x35,
	kNPlayerList = 0xC9,
	kNPlugin = 0xFA,
	kNKick = 0xFF,
};

struct Cursor
{
	const uint8_t *p;
	std::size_t left;
	bool ok = true;

	Cursor(const uint8_t *data, std::size_t len) : p(data), left(len) {}

	uint8_t u8();
	int8_t i8();
	uint16_t u16();
	int16_t i16();
	int32_t i32();
	int64_t i64();
	float f32();
	double f64();
	uint32_t varint(); // unsigned (ids, counts, keepalive echo)
	int32_t svarint(); // signed
	// 1.2.5 string: short char count + UTF-16BE, returned as UTF-8.
	std::string str125(std::size_t maxChars);
	// 1.8 string: VarInt byte count + UTF-8.
	std::string str189();
	void skip(std::size_t n);
};

struct Writer
{
	std::vector<uint8_t> b;
	void u8(uint8_t v);
	void i8(int8_t v);
	void u16(uint16_t v);
	void i16(int16_t v);
	void i32(int32_t v);
	void i64(int64_t v);
	void f32(float v);
	void f64(double v);
	void varint(uint32_t v);
	void raw(const uint8_t *data, std::size_t len);
	void raw(const std::vector<uint8_t> &v);
	// 1.2.5 string from UTF-8, truncated to maxChars UTF-16 units.
	void str125(const std::string &utf8, std::size_t maxChars);
	// 1.8 string from UTF-8 (bytes used as-is).
	void str189(const std::string &utf8);
};

// 1.8 64-bit Position: x(26) | y(12) | z(26).
int64_t encodePosition(int32_t x, int32_t y, int32_t z);
void decodePosition(int64_t v, int32_t &x, int32_t &y, int32_t &z);

// 1.8 JSON chat -> plain text, legacy section codes preserved.
std::string chatToText(const std::string &json);
// MOTD sanitizer for the 1.2.5 ping reply (no section char/newlines).
std::string sanitizeMotd(const std::string &text);
// Truncate UTF-8 to at most maxCodePoints Unicode code points.
std::string truncateUtf8(const std::string &text, std::size_t maxCodePoints);

// Skip one 1.8 NBT blob starting at p (leading 0x00 = empty). Returns the
// blob length, 0 on malformed input.
std::size_t skipNbt(const uint8_t *p, std::size_t len);

// 1.8 slot -> 1.2.5 slot field writer (id, count, damage, NBT re-wrapped
// with the short length prefix). Returns false on malformed input.
bool convertSlot189To125(Cursor &c, Writer &w);
// 1.2.5 slot -> 1.8 slot.
bool convertSlot125To189(Cursor &c, Writer &w);

// 1.8 section layout -> 1.2.5 ground-up raw (UNCOMPRESSED; the IO layer
// compresses with zlib because the native Packet51 decoder inflates).
// data holds exactly the section bytes (biome split off by the caller).
// skylight=false (nether/end) fills 0xFF sky arrays.
bool convertChunkSections(uint16_t bitmask, const uint8_t *data, std::size_t len,
                          bool skylight, uint16_t &primaryOut, uint16_t &addOut,
                          std::vector<uint8_t> &rawOut);

// One converted 1.8 chunk column (raw, uncompressed).
struct ConvertedChunk
{
	int32_t x = 0;
	int32_t z = 0;
	bool groundUp = false;
	uint16_t primary = 0;
	uint16_t add = 0;
	std::vector<uint8_t> raw;   // section bytes (+ 256 biome iff groundUp)
	std::vector<uint8_t> biome; // 256 bytes iff groundUp, else empty
};

// A synthesized native 1.2.5 packet: id byte + wire payload (no framing).
struct NativePacket
{
	uint8_t id = 0;
	std::vector<uint8_t> payload;
};

// Per-connection translation state. Written by the reader thread
// (threshold, eid, stage) and the writer thread (player position);
// atomics cover the former, a mutex the latter.
struct Session189
{
	enum class Stage
	{
		Idle,     // native handshake not seen yet
		Login,    // 1.8 Login Start sent, awaiting Login Success
		Play,     // translating gameplay
	};

	std::atomic<int> stage{static_cast<int>(Stage::Idle)};
	std::atomic<int> compressionThreshold{-1};
	std::atomic<int32_t> entityId{-1};
	std::atomic<int> dimension{0};
	std::string username;
	std::mutex posMutex;
	double posX = 0.0;
	double posY = 0.0;
	double posZ = 0.0;
	float yaw = 0.0f;
	float pitch = 0.0f;
};

// Complete 1.8 frames (length-prefixed) for the login open.
std::vector<uint8_t> encodeHandshake189(const std::string &host, uint16_t port,
                                        uint32_t nextState);
std::vector<uint8_t> encodeLoginStart189(const std::string &username);

// Serverbound: native 1.2.5 packet (id + payload, no framing) -> zero or
// more complete 1.8 frames. Returns false when the packet must be dropped
// (login swallowed, quit, unmapped) with no error; throws on malformed.
bool translateServerbound(Session189 &s, uint8_t nativeId,
                          const uint8_t *payload, std::size_t len,
                          std::vector<std::vector<uint8_t>> &framesOut);

// Clientbound: one 1.8 packet (id + payload, length already stripped) ->
// zero or more native 1.2.5 packets. Unmapped/error packets are dropped
// (never throw for unknown play ids: framing already consumed them).
// A chunk yields two packets: prechunk first, then mapchunk (raw inside
// ConvertedChunk, still uncompressed).
struct ClientboundOut
{
	std::vector<NativePacket> packets;
	bool hasChunk = false;
	ConvertedChunk chunk;
};
void translateClientbound(Session189 &s, uint32_t id189,
                          const uint8_t *payload, std::size_t len,
                          ClientboundOut &out);

// 1.8 status JSON -> 1.2.5 ping fields.
bool statusTo125(const std::string &statusJson, std::string &motdOut,
                 int &onlineOut, int &maxOut);

} // namespace translator189
