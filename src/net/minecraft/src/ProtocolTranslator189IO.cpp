// ProtocolTranslator189IO -- see the header for the contract.

#include "ProtocolTranslator189IO.h"

#include <chrono>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>

#include <zlib.h>

#include "Packet.h"
#include "java/JavaNetwork.h"

namespace
{

struct Eof : std::exception
{
};

constexpr std::size_t kMaxFrameBytes = 2 * 1024 * 1024; // 1.8 chunk bound
constexpr std::size_t kMaxDecompressedBytes = 4 * 1024 * 1024;

int streamGet(std::istream &is)
{
	const int v = is.get();
	if (v == std::char_traits<char>::eof())
		throw Eof();
	return v;
}

uint32_t readVarintStream(std::istream &is)
{
	uint32_t value = 0;
	for (int i = 0; i < 5; i++)
	{
		const int b = streamGet(is);
		value |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
		if (!(b & 0x80))
			return value;
	}
	throw std::runtime_error("1.8.9 frame varint too long");
}

void readExactStream(std::istream &is, uint8_t *dst, std::size_t n)
{
	std::size_t got = 0;
	while (got < n)
	{
		is.read(reinterpret_cast<char *>(dst + got), static_cast<std::streamsize>(n - got));
		const std::streamsize just = is.gcount();
		if (just <= 0)
			throw Eof();
		got += static_cast<std::size_t>(just);
	}
}

bool zlibCompress(const std::vector<uint8_t> &in, std::vector<uint8_t> &out)
{
	uLongf bound = compressBound(static_cast<uLong>(in.size()));
	out.resize(static_cast<std::size_t>(bound));
	const int rc = compress2(out.data(), &bound, in.data(),
	                         static_cast<uLong>(in.size()), Z_DEFAULT_COMPRESSION);
	if (rc != Z_OK)
	{
		out.clear();
		return false;
	}
	out.resize(static_cast<std::size_t>(bound));
	return true;
}

bool zlibDecompress(const uint8_t *in, std::size_t inLen, uint32_t expectLen,
                    std::vector<uint8_t> &out)
{
	if (expectLen == 0 || expectLen > kMaxDecompressedBytes)
		return false;
	out.resize(expectLen);
	uLongf actual = expectLen;
	const int rc = uncompress(out.data(), &actual, in, static_cast<uLong>(inLen));
	if (rc != Z_OK || actual != expectLen)
	{
		out.clear();
		return false;
	}
	return true;
}

void emitFrame(std::ostream &os, int threshold, uint32_t id,
               const std::vector<uint8_t> &payload)
{
	translator189::Writer body;
	body.varint(id);
	body.raw(payload);
	translator189::Writer frame;
	if (threshold >= 0)
	{
		if (body.b.size() >= static_cast<std::size_t>(threshold))
		{
			std::vector<uint8_t> comp;
			if (!zlibCompress(body.b, comp))
				throw std::runtime_error("1.8.9 compress failed");
			translator189::Writer inner;
			inner.varint(static_cast<uint32_t>(body.b.size()));
			inner.raw(comp);
			frame.varint(static_cast<uint32_t>(inner.b.size()));
			frame.raw(inner.b);
		}
		else
		{
			translator189::Writer inner;
			inner.varint(0);
			inner.raw(body.b);
			frame.varint(static_cast<uint32_t>(inner.b.size()));
			frame.raw(inner.b);
		}
	}
	else
	{
		frame.varint(static_cast<uint32_t>(body.b.size()));
		frame.raw(body.b);
	}
	os.write(reinterpret_cast<const char *>(frame.b.data()),
	         static_cast<std::streamsize>(frame.b.size()));
	if (!os)
		throw std::runtime_error("Failed to write 1.8.9 frame");
}

std::unique_ptr<Packet> decodeNative(const uint8_t *wire, std::size_t len)
{
	std::string bytes(reinterpret_cast<const char *>(wire), len);
	std::istringstream iss(bytes);
	auto packet = Packet::readPacket(iss, false);
	if (!packet)
		throw std::runtime_error("Translator produced an undecodable packet");
	return packet;
}

} // namespace

Translator189Connection::Translator189Connection(const std::string &host, int port)
    : host_(host), port_(port)
{
}

std::unique_ptr<Packet> Translator189Connection::readOneTranslated(std::istream &is)
{
	if (!pending_.empty())
	{
		auto packet = std::move(pending_.front());
		pending_.pop_front();
		return packet;
	}
	for (;;)
	{
		uint32_t total = 0;
		try
		{
			total = readVarintStream(is);
		}
		catch (const Eof &)
		{
			return nullptr;
		}
		if (total == 0 || total > kMaxFrameBytes)
			throw std::runtime_error("Bad 1.8.9 frame length");
		std::vector<uint8_t> frame(total);
		try
		{
			readExactStream(is, frame.data(), total);
		}
		catch (const Eof &)
		{
			throw std::runtime_error("Truncated 1.8.9 frame");
		}
		const int threshold =
		    session_.compressionThreshold.load(std::memory_order_acquire);
		translator189::Cursor c(frame.data(), frame.size());
		if (threshold >= 0)
		{
			const uint32_t dataLen = c.varint();
			if (!c.ok)
				throw std::runtime_error("Bad 1.8.9 compression header");
			if (dataLen == 0)
			{
				// Uncompressed: remainder is the packet as-is.
			}
			else
			{
				std::vector<uint8_t> plain;
				if (!zlibDecompress(c.p, c.left, dataLen, plain))
					throw std::runtime_error("Bad 1.8.9 compressed block");
				c = translator189::Cursor(plain.data(), plain.size());
				// Keep `plain` alive through the translate call below.
				translator189::ClientboundOut bout;
				const uint32_t id = c.varint();
				if (!c.ok)
					throw std::runtime_error("Bad 1.8.9 packet id");
				translator189::translateClientbound(session_, id, c.p, c.left, bout);
				if (!appendDecoded(bout))
					continue;
				return takePending();
			}
		}
		translator189::ClientboundOut bout;
		const uint32_t id = c.varint();
		if (!c.ok)
			throw std::runtime_error("Bad 1.8.9 packet id");
		translator189::translateClientbound(session_, id, c.p, c.left, bout);
		if (!appendDecoded(bout))
			continue;
		return takePending();
	}
}

std::unique_ptr<Packet> Translator189Connection::takePending()
{
	auto packet = std::move(pending_.front());
	pending_.pop_front();
	return packet;
}

bool Translator189Connection::appendDecoded(translator189::ClientboundOut &bout)
{
	for (const translator189::NativePacket &np : bout.packets)
	{
		std::vector<uint8_t> wire;
		wire.reserve(np.payload.size() + 1);
		wire.push_back(np.id);
		wire.insert(wire.end(), np.payload.begin(), np.payload.end());
		pending_.push_back(decodeNative(wire.data(), wire.size()));
	}
	if (bout.hasChunk)
	{
		// Prechunk first (wire order matters to the handler)...
		translator189::Writer pre;
		pre.i32(bout.chunk.x);
		pre.i32(bout.chunk.z);
		pre.u8(1);
		std::vector<uint8_t> preWire;
		preWire.push_back(translator189::kNPreChunk);
		preWire.insert(preWire.end(), pre.b.begin(), pre.b.end());
		pending_.push_back(decodeNative(preWire.data(), preWire.size()));
		// ...then the map chunk with the zlib payload the native
		// Packet51 decoder inflates itself (keeping every platform's
		// deferred/import profile intact).
		std::vector<uint8_t> raw = bout.chunk.raw;
		raw.insert(raw.end(), bout.chunk.biome.begin(), bout.chunk.biome.end());
		std::vector<uint8_t> comp;
		if (!zlibCompress(raw, comp))
			throw std::runtime_error("1.8.9->1.2.5 chunk compress failed");
		translator189::Writer map;
		map.i32(bout.chunk.x);
		map.i32(bout.chunk.z);
		map.u8(bout.chunk.groundUp ? 1 : 0);
		map.i16(static_cast<int16_t>(bout.chunk.primary));
		map.i16(static_cast<int16_t>(bout.chunk.add));
		map.i32(static_cast<int32_t>(comp.size()));
		map.raw(comp);
		std::vector<uint8_t> mapWire;
		mapWire.push_back(translator189::kNMapChunk);
		mapWire.insert(mapWire.end(), map.b.begin(), map.b.end());
		pending_.push_back(decodeNative(mapWire.data(), mapWire.size()));
	}
	return !pending_.empty();
}

bool Translator189Connection::writeTranslated(Packet *packet, std::ostream &os)
{
	if (packet == nullptr)
		return false;
	std::ostringstream oss;
	Packet::writePacket(packet, oss);
	const std::string &bytes = oss.str();
	if (bytes.empty())
		return false;
	const uint8_t nativeId = static_cast<uint8_t>(bytes[0]);
	const uint8_t *payload = reinterpret_cast<const uint8_t *>(bytes.data()) + 1;
	const std::size_t len = bytes.size() - 1;

	const int stage = session_.stage.load(std::memory_order_acquire);
	if (stage == static_cast<int>(translator189::Session189::Stage::Idle) &&
	    nativeId == translator189::kNHandshake)
	{
		// Open the 1.8.9 login: handshake + Login Start carry the
		// username the native handshake packet holds.
		translator189::Cursor c(payload, len);
		const std::string username = c.str125(64);
		if (!c.ok || c.left != 0 || username.empty())
			throw std::runtime_error("Bad native handshake packet");
		session_.username = username;
		const std::vector<uint8_t> hs =
		    translator189::encodeHandshake189(host_, static_cast<uint16_t>(port_), 2);
		os.write(reinterpret_cast<const char *>(hs.data()),
		         static_cast<std::streamsize>(hs.size()));
		const std::vector<uint8_t> ls =
		    translator189::encodeLoginStart189(username);
		os.write(reinterpret_cast<const char *>(ls.data()),
		         static_cast<std::streamsize>(ls.size()));
		if (!os)
			throw std::runtime_error("Failed to write 1.8.9 login open");
		session_.stage.store(
		    static_cast<int>(translator189::Session189::Stage::Login),
		    std::memory_order_release);
		return true;
	}

	std::vector<std::vector<uint8_t>> frames;
	// Note: translateServerbound owns the Idle/Login swallow rules; the
	// handshake branch above only exists because it needs host/port.
	if (!translator189::translateServerbound(session_, nativeId, payload, len, frames))
		return false;
	const int threshold =
	    session_.compressionThreshold.load(std::memory_order_acquire);
	for (const std::vector<uint8_t> &frame : frames)
	{
		translator189::Cursor c(frame.data(), frame.size());
		const uint32_t id = c.varint();
		std::vector<uint8_t> body(c.p, c.p + c.left);
		emitFrame(os, threshold, id, body);
	}
	return !frames.empty();
}

bool Translator189Connection::pollStatus(const std::string &host, int port,
                                         std::string &motdOut, int &onlineOut,
                                         int &maxOut, long long &lagMsOut)
{
	motdOut.clear();
	onlineOut = 0;
	maxOut = 0;
	lagMsOut = 0;
	std::unique_ptr<JavaNetwork::Socket> socket = JavaNetwork::createSocket();
	if (socket == nullptr || !socket->connect(host, port))
		return false;

	auto writeAll = [&](const std::vector<uint8_t> &v) {
		return socket->write(reinterpret_cast<const char *>(v.data()),
		                     static_cast<int>(v.size()));
	};
	auto readExact = [&](uint8_t *dst, std::size_t n) {
		std::size_t got = 0;
		while (got < n)
		{
			const int just = socket->read(reinterpret_cast<char *>(dst + got),
			                              static_cast<int>(n - got));
			if (just <= 0)
				return false;
			got += static_cast<std::size_t>(just);
		}
		return true;
	};
	auto readVarint = [&](uint32_t &v) {
		v = 0;
		for (int i = 0; i < 5; i++)
		{
			uint8_t b = 0;
			if (!readExact(&b, 1))
				return false;
			v |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
			if (!(b & 0x80))
				return true;
		}
		return false;
	};

	const auto start = std::chrono::steady_clock::now();
	if (!writeAll(translator189::encodeHandshake189(host, static_cast<uint16_t>(port), 1)))
		return false;
	{
		translator189::Writer body;
		body.varint(0x00);
		translator189::Writer frame;
		frame.varint(static_cast<uint32_t>(body.b.size()));
		frame.raw(body.b);
		if (!writeAll(frame.b))
			return false;
	}
	uint32_t total = 0;
	if (!readVarint(total) || total == 0 || total > kMaxFrameBytes)
		return false;
	std::vector<uint8_t> frame(total);
	if (!readExact(frame.data(), total))
		return false;
	translator189::Cursor c(frame.data(), frame.size());
	if (c.varint() != 0x00 || !c.ok)
		return false;
	const std::string statusJson = c.str189();
	if (!c.ok)
		return false;
	if (!translator189::statusTo125(statusJson, motdOut, onlineOut, maxOut))
		return false;

	const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
	                            std::chrono::system_clock::now().time_since_epoch())
	                            .count();
	translator189::Writer pingBody;
	pingBody.varint(0x01);
	pingBody.i64(nowMs);
	translator189::Writer pingFrame;
	pingFrame.varint(static_cast<uint32_t>(pingBody.b.size()));
	pingFrame.raw(pingBody.b);
	if (!writeAll(pingFrame.b))
		return false;
	if (!readVarint(total) || total == 0 || total > 64)
		return false;
	std::vector<uint8_t> pong(total);
	if (!readExact(pong.data(), total))
		return false;
	lagMsOut = std::chrono::duration_cast<std::chrono::milliseconds>(
	               std::chrono::steady_clock::now() - start)
	               .count();
	socket->close();
	return true;
}
