// ProtocolTranslator189Tests -- unit checks for the translation core.
// Mirrors proxy/selftest.py's unit section (the end-to-end half needs live
// sockets and stays in Python). Build: ctest -R ProtocolTranslator189Tests.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net/minecraft/src/ProtocolTranslator189.h"

namespace T = translator189;

namespace
{
int failures = 0;

void check(bool cond, const char *name)
{
	std::printf("%s: %s\n", cond ? "ok" : "FAIL", name);
	if (!cond)
		failures++;
}

T::Cursor cursorOf(const std::vector<uint8_t> &v)
{
	return T::Cursor(v.data(), v.size());
}

// Build a 1.8 section: 4096 big-endian (id<<4|meta), 2048 light, 2048 sky.
std::vector<uint8_t> makeSection189(int id, int meta, uint8_t light, uint8_t sky)
{
	std::vector<uint8_t> out;
	const uint16_t v = static_cast<uint16_t>((id << 4) | meta);
	for (int i = 0; i < 4096; i++)
	{
		out.push_back(static_cast<uint8_t>(v >> 8));
		out.push_back(static_cast<uint8_t>(v & 0xFF));
	}
	out.insert(out.end(), 2048, light);
	out.insert(out.end(), 2048, sky);
	return out;
}

} // namespace

int main()
{
	// VarInt round-trips.
	{
		T::Writer w;
		w.varint(300);
		T::Cursor c = cursorOf(w.b);
		check(c.varint() == 300 && c.left == 0, "varint 300");
	}
	{
		T::Writer w;
		w.varint(0xFFFFFFFFu);
		T::Cursor c = cursorOf(w.b);
		check(c.varint() == 0xFFFFFFFFu && c.left == 0, "varint max");
	}
	// 1.2.5 strings incl. a non-BMP char (surrogate pair = 2 units).
	{
		T::Writer w;
		w.str125("A\xF0\x9F\x98\x80" "B", 64); // A + U+1F600 + B
		T::Cursor c = cursorOf(w.b);
		check(c.str125(64) == "A\xF0\x9F\x98\x80" "B", "str125 astral");
	}
	{
		T::Writer w;
		w.str125("abcdef", 4);
		T::Cursor c = cursorOf(w.b);
		check(c.str125(64) == "abcd", "str125 truncates to maxChars");
	}
	{
		T::Writer w;
		w.str189("hola");
		T::Cursor c = cursorOf(w.b);
		check(c.str189() == "hola", "str189 roundtrip");
	}
	// Position incl. negatives (the selftest-caught Y/Z layout).
	{
		const int xs[] = {0, -5, 30000000};
		const int ys[] = {64, -3, 255};
		const int zs[] = {0, 129, -30000000};
		bool ok = true;
		for (int i = 0; i < 3; i++)
		{
			int x, y, z;
			T::decodePosition(T::encodePosition(xs[i], ys[i], zs[i]), x, y, z);
			ok = ok && x == xs[i] && y == ys[i] && z == zs[i];
		}
		check(ok, "position roundtrip");
	}
	// JSON chat strip.
	check(T::chatToText("{\"text\":\"hi\",\"extra\":[{\"text\":\" there\"}]}") == "hi there",
	      "chat extra");
	check(T::chatToText("plain") == "plain", "chat non-json passthrough");
	// Status JSON.
	{
		std::string motd;
		int online = 0, max = 0;
		const bool ok = T::statusTo125(
		    "{\"description\":{\"text\":\"Test MOTD\"},\"players\":{\"online\":5,\"max\":20}}",
		    motd, online, max);
		check(ok && motd == "Test MOTD" && online == 5 && max == 20, "statusTo125");
	}
	// NBT skip: compound { string "name" = "AB" }.
	{
		const uint8_t nbt[] = {10, 0, 0, 8, 0, 4, 'n', 'a', 'm', 'e', 0, 2, 'A', 'B', 0};
		check(T::skipNbt(nbt, sizeof(nbt)) == sizeof(nbt), "nbt compound skip");
		const uint8_t empty[] = {0};
		check(T::skipNbt(empty, 1) == 1, "nbt empty skip");
		const uint8_t bad[] = {7, 0, 0};
		check(T::skipNbt(bad, sizeof(bad)) == 0, "nbt malformed");
	}
	// Slots both ways.
	{
		T::Writer in189;
		in189.i16(5);
		in189.u8(3);
		in189.i16(0);
		in189.u8(0); // no NBT
		T::Cursor c = cursorOf(in189.b);
		T::Writer out125;
		check(T::convertSlot189To125(c, out125) && c.left == 0, "slot 189->125");
		T::Cursor c2 = cursorOf(out125.b);
		T::Writer back189;
		check(T::convertSlot125To189(c2, back189) && back189.b == in189.b,
		      "slot 125->189 roundtrip");
	}
	{
		T::Writer w;
		w.i16(-1);
		T::Cursor c = cursorOf(w.b);
		T::Writer o;
		check(T::convertSlot189To125(c, o), "empty slot");
	}
	// Chunk sections: stone + full sky, then an id>255 Add case.
	{
		const std::vector<uint8_t> sec = makeSection189(1, 0, 0x00, 0xFF);
		uint16_t primary = 0, add = 0;
		std::vector<uint8_t> raw;
		check(T::convertChunkSections(0x0001, sec.data(), sec.size(), true, primary, add, raw),
		      "chunk convert ok");
		check(primary == 1 && add == 0, "chunk masks");
		bool idsOk = true, metaOk = true, skyOk = true;
		for (int i = 0; i < 4096; i++)
			idsOk = idsOk && raw[i] == 1;
		for (int i = 4096; i < 6144; i++)
			metaOk = metaOk && raw[i] == 0;
		for (int i = 4096 + 4096; i < 4096 + 6144; i++)
			skyOk = skyOk && raw[i] == 0xFF;
		check(idsOk && metaOk && skyOk, "chunk section bytes");
	}
	{
		std::vector<uint8_t> sec = makeSection189(300, 0, 0x00, 0xFF);
		uint16_t primary = 0, add = 0;
		std::vector<uint8_t> raw;
		check(T::convertChunkSections(0x0001, sec.data(), sec.size(), true, primary, add, raw) &&
		      add == 1,
		      "chunk Add mask for id>255");
		check(raw[0] == (300 & 0xFF) && raw[10240] == 0x11,
		      "chunk Add low/high split");
	}
	// Serverbound: handshake opens login, login swallowed, chat/move work.
	{
		T::Session189 s;
		T::Writer hs;
		hs.str125("Steve", 64);
		std::vector<std::vector<uint8_t>> frames;
		check(T::translateServerbound(s, T::kNHandshake, hs.b.data(), hs.b.size(), frames) &&
		      frames.empty() &&
		      s.stage.load() == static_cast<int>(T::Session189::Stage::Login),
		      "handshake opens login, nothing forwarded");
		check(T::translateServerbound(s, T::kNLogin, nullptr, 0, frames),
		      "native login swallowed");
		s.stage.store(static_cast<int>(T::Session189::Stage::Play));
		T::Writer chat;
		chat.str125("hi", 119);
		check(T::translateServerbound(s, T::kNChat, chat.b.data(), chat.b.size(), frames) &&
		      frames.size() == 1,
		      "chat emits one frame");
		T::Cursor fc = cursorOf(frames.back());
		const uint32_t total = fc.varint();
		(void)total;
		check(fc.varint() == 0x01, "chat frame id");
	}
	// Clientbound: keepalive, joingame, login-success handshake, chunk.
	{
		T::Session189 s;
		s.stage.store(static_cast<int>(T::Session189::Stage::Login));
		T::ClientboundOut out;
		T::Writer succ;
		succ.str189("00000000-0000-0000-0000-000000000000");
		succ.str189("Steve");
		T::translateClientbound(s, T::kLoginSuccess, succ.b.data(), succ.b.size(), out);
		check(out.packets.size() == 1 && out.packets[0].id == T::kNHandshake &&
		      s.stage.load() == static_cast<int>(T::Session189::Stage::Play),
		      "login success -> handshake + play stage");
		T::Writer join;
		join.i32(1234);
		join.u8(1);
		join.i8(0);
		join.u8(1);
		join.u8(10);
		join.str189("default");
		out.packets.clear();
		T::translateClientbound(s, T::kJoinGame, join.b.data(), join.b.size(), out);
		check(out.packets.size() == 1 && out.packets[0].id == T::kNLogin,
		      "joingame -> login");
		T::Cursor jc(out.packets[0].payload.data(), out.packets[0].payload.size());
		check(jc.i32() == 1234, "login carries eid");
	}
	{
		T::Session189 s;
		s.stage.store(static_cast<int>(T::Session189::Stage::Play));
		const std::vector<uint8_t> sec = makeSection189(1, 0, 0x00, 0xFF);
		T::Writer chunk;
		chunk.i32(0);
		chunk.i32(0);
		chunk.u8(1);
		chunk.u16(1);
		std::vector<uint8_t> data = sec;
		data.insert(data.end(), 256, 0); // biome
		chunk.varint(static_cast<uint32_t>(data.size()));
		chunk.raw(data);
		T::ClientboundOut out;
		T::translateClientbound(s, T::kChunkData, chunk.b.data(), chunk.b.size(), out);
		check(out.packets.size() == 1 && out.packets[0].id == T::kNPreChunk && out.hasChunk,
		      "chunk -> prechunk + converted");
		check(out.chunk.primary == 1 && out.chunk.raw.size() == 4096 + 2048 + 2048 + 2048 &&
		      out.chunk.biome.size() == 256,
		      "chunk raw sizes");
		check(out.chunk.raw[0] == 1, "chunk raw content");
	}
	{
		T::Session189 s;
		s.stage.store(static_cast<int>(T::Session189::Stage::Play));
		T::Writer pl;
		pl.varint(0);
		pl.varint(1);
		for (int i = 0; i < 16; i++)
			pl.u8(0);
		pl.str189("Steve");
		pl.varint(0);
		pl.varint(0);
		pl.varint(10);
		pl.u8(0);
		T::ClientboundOut out;
		T::translateClientbound(s, T::kPlayerList, pl.b.data(), pl.b.size(), out);
		check(out.packets.size() == 1 && out.packets[0].id == T::kNPlayerList,
		      "playerlist add");
	}

	std::printf(failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
	return failures == 0 ? 0 : 1;
}
