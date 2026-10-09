// ProtocolTranslator189 core -- see the header for the design contract.
// Ported from the verified proxy/*.py MVP (selftest 37/37); the three bugs
// the selftest caught there (Position Y/Z layout, groundUp biome split,
// 0x35 size) are already correct here.

#include "ProtocolTranslator189.h"

#include <cstdio>
#include <cstring>

#include "AuthJson.h"

namespace translator189
{
namespace
{

void appendU16be(Writer &w, uint16_t v)
{
	w.u8(static_cast<uint8_t>(v >> 8));
	w.u8(static_cast<uint8_t>(v & 0xFF));
}

// Decode one UTF-16BE unit; advances *p. Returns the code point and sets
// *units to 1 or 2 (surrogate pair).
uint32_t decodeUtf16Unit(const uint8_t *&p, std::size_t &left, int &units)
{
	const uint16_t hi = static_cast<uint16_t>((p[0] << 8) | p[1]);
	p += 2;
	left -= 2;
	units = 1;
	if (hi >= 0xD800 && hi <= 0xDBFF && left >= 2)
	{
		const uint16_t lo = static_cast<uint16_t>((p[0] << 8) | p[1]);
		if (lo >= 0xDC00 && lo <= 0xDFFF)
		{
			p += 2;
			left -= 2;
			units = 2;
			return 0x10000u + ((static_cast<uint32_t>(hi) - 0xD800u) << 10) +
			       (static_cast<uint32_t>(lo) - 0xDC00u);
		}
	}
	return hi;
}

void encodeUtf8(Writer &w, uint32_t cp)
{
	if (cp < 0x80)
		w.u8(static_cast<uint8_t>(cp));
	else if (cp < 0x800)
	{
		w.u8(static_cast<uint8_t>(0xC0 | (cp >> 6)));
		w.u8(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
	}
	else if (cp < 0x10000)
	{
		w.u8(static_cast<uint8_t>(0xE0 | (cp >> 12)));
		w.u8(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
		w.u8(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
	}
	else
	{
		w.u8(static_cast<uint8_t>(0xF0 | (cp >> 18)));
		w.u8(static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F)));
		w.u8(static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F)));
		w.u8(static_cast<uint8_t>(0x80 | (cp & 0x3F)));
	}
}

void encodeUtf16be(Writer &w, uint32_t cp)
{
	if (cp < 0x10000)
	{
		appendU16be(w, static_cast<uint16_t>(cp));
	}
	else
	{
		cp -= 0x10000u;
		appendU16be(w, static_cast<uint16_t>(0xD800 + (cp >> 10)));
		appendU16be(w, static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
	}
}

// Length of one UTF-8 code point starting at lead byte (0 on invalid).
std::size_t utf8PointLen(uint8_t lead)
{
	if (lead < 0x80)
		return 1;
	if ((lead & 0xE0) == 0xC0)
		return 2;
	if ((lead & 0xF0) == 0xE0)
		return 3;
	if ((lead & 0xF8) == 0xF0)
		return 4;
	return 0;
}

uint32_t decodeUtf8Point(const uint8_t *p, std::size_t len)
{
	if (len == 1)
		return p[0];
	if (len == 2)
		return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
	if (len == 3)
		return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
	return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
	       ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

void framePlain189(uint32_t id, const std::vector<uint8_t> &payload,
                   std::vector<uint8_t> &frame)
{
	Writer body;
	body.varint(id);
	body.raw(payload);
	Writer out;
	out.varint(static_cast<uint32_t>(body.b.size()));
	out.raw(body.b);
	frame = std::move(out.b);
}

void chatWalk(const AuthJson::Value &node, std::string &out)
{
	if (node.type == AuthJson::Value::Type::String)
	{
		out += node.text;
		return;
	}
	if (node.type != AuthJson::Value::Type::Object)
		return;
	if (const AuthJson::Value *text = node.member("text"))
		if (text->type == AuthJson::Value::Type::String)
			out += text->text;
	if (const AuthJson::Value *extra = node.member("extra"))
		if (extra->type == AuthJson::Value::Type::Array)
			for (const AuthJson::Value &child : extra->array)
				chatWalk(child, out);
}

// 1.2.5 entity-action -> 1.8 entity-action.
bool mapEntityAction(uint8_t in, uint32_t &out)
{
	switch (in)
	{
	case 1:
		out = 0;
		return true;
	case 2:
		out = 1;
		return true;
	case 3:
		out = 2;
		return true;
	case 4:
		out = 3;
		return true;
	case 5:
		out = 4;
		return true;
	default:
		return false;
	}
}

} // namespace

// -- Cursor ---------------------------------------------------------------

uint8_t Cursor::u8()
{
	if (left < 1)
	{
		ok = false;
		return 0;
	}
	const uint8_t v = *p;
	++p;
	--left;
	return v;
}

int8_t Cursor::i8()
{
	return static_cast<int8_t>(u8());
}

uint16_t Cursor::u16()
{
	if (left < 2)
	{
		ok = false;
		return 0;
	}
	const uint16_t v = static_cast<uint16_t>((p[0] << 8) | p[1]);
	p += 2;
	left -= 2;
	return v;
}

int16_t Cursor::i16()
{
	return static_cast<int16_t>(u16());
}

int32_t Cursor::i32()
{
	if (left < 4)
	{
		ok = false;
		return 0;
	}
	const int32_t v = static_cast<int32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
	p += 4;
	left -= 4;
	return v;
}

int64_t Cursor::i64()
{
	if (left < 8)
	{
		ok = false;
		return 0;
	}
	int64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	p += 8;
	left -= 8;
	return v;
}

float Cursor::f32()
{
	const int32_t bits = i32();
	float v = 0.0f;
	std::memcpy(&v, &bits, 4);
	return v;
}

double Cursor::f64()
{
	const int64_t bits = i64();
	double v = 0.0;
	std::memcpy(&v, &bits, 8);
	return v;
}

uint32_t Cursor::varint()
{
	uint32_t value = 0;
	for (int i = 0; i < 5; i++)
	{
		const uint8_t b = u8();
		if (!ok)
			return 0;
		value |= static_cast<uint32_t>(b & 0x7F) << (7 * i);
		if (!(b & 0x80))
			return value;
	}
	ok = false;
	return 0;
}

int32_t Cursor::svarint()
{
	return static_cast<int32_t>(varint());
}

std::string Cursor::str125(std::size_t maxChars)
{
	const int16_t n = i16();
	if (!ok || n < 0 || static_cast<std::size_t>(n) > maxChars)
	{
		ok = false;
		return "";
	}
	if (left < static_cast<std::size_t>(n) * 2)
	{
		ok = false;
		return "";
	}
	Writer w;
	int units = 0;
	while (units < n)
	{
		if (left < 2)
		{
			ok = false;
			return "";
		}
		int got = 1;
		const uint32_t cp = decodeUtf16Unit(p, left, got);
		units += got;
		if (units > n)
		{
			ok = false;
			return "";
		}
		encodeUtf8(w, cp);
	}
	return std::string(w.b.begin(), w.b.end());
}

std::string Cursor::str189()
{
	const uint32_t n = varint();
	if (!ok || n > left)
	{
		ok = false;
		return "";
	}
	std::string s(reinterpret_cast<const char *>(p), n);
	p += n;
	left -= n;
	return s;
}

void Cursor::skip(std::size_t n)
{
	if (left < n)
	{
		ok = false;
		return;
	}
	p += n;
	left -= n;
}

// -- Writer ---------------------------------------------------------------

void Writer::u8(uint8_t v)
{
	b.push_back(v);
}

void Writer::i8(int8_t v)
{
	b.push_back(static_cast<uint8_t>(v));
}

void Writer::u16(uint16_t v)
{
	appendU16be(*this, v);
}

void Writer::i16(int16_t v)
{
	appendU16be(*this, static_cast<uint16_t>(v));
}

void Writer::i32(int32_t v)
{
	const uint32_t u = static_cast<uint32_t>(v);
	u8(static_cast<uint8_t>(u >> 24));
	u8(static_cast<uint8_t>((u >> 16) & 0xFF));
	u8(static_cast<uint8_t>((u >> 8) & 0xFF));
	u8(static_cast<uint8_t>(u & 0xFF));
}

void Writer::i64(int64_t v)
{
	const uint64_t u = static_cast<uint64_t>(v);
	for (int i = 7; i >= 0; i--)
		u8(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
}

void Writer::f32(float v)
{
	int32_t bits = 0;
	std::memcpy(&bits, &v, 4);
	i32(bits);
}

void Writer::f64(double v)
{
	int64_t bits = 0;
	std::memcpy(&bits, &v, 8);
	i64(bits);
}

void Writer::varint(uint32_t v)
{
	while (true)
	{
		const uint8_t chunk = static_cast<uint8_t>(v & 0x7F);
		v >>= 7;
		if (v)
			u8(static_cast<uint8_t>(chunk | 0x80));
		else
		{
			u8(chunk);
			return;
		}
	}
}

void Writer::raw(const uint8_t *data, std::size_t len)
{
	b.insert(b.end(), data, data + len);
}

void Writer::raw(const std::vector<uint8_t> &v)
{
	b.insert(b.end(), v.begin(), v.end());
}

void Writer::str125(const std::string &utf8, std::size_t maxChars)
{
	// First pass: count UTF-16 units so the length prefix is exact, then
	// emit at most maxChars units without splitting a surrogate pair.
	const uint8_t *p = reinterpret_cast<const uint8_t *>(utf8.data());
	std::size_t left = utf8.size();
	std::size_t units = 0;
	while (left > 0 && units < maxChars)
	{
		const std::size_t len = utf8PointLen(*p);
		if (len == 0 || len > left)
			break;
		const uint32_t cp = decodeUtf8Point(p, len);
		const std::size_t need = cp < 0x10000 ? 1 : 2;
		if (units + need > maxChars)
			break;
		units += need;
		p += len;
		left -= len;
	}
	i16(static_cast<int16_t>(units));
	p = reinterpret_cast<const uint8_t *>(utf8.data());
	left = utf8.size();
	std::size_t done = 0;
	while (left > 0 && done < units)
	{
		const std::size_t len = utf8PointLen(*p);
		if (len == 0 || len > left)
			break;
		const uint32_t cp = decodeUtf8Point(p, len);
		const std::size_t need = cp < 0x10000 ? 1 : 2;
		if (done + need > units)
			break;
		encodeUtf16be(*this, cp);
		done += need;
		p += len;
		left -= len;
	}
}

void Writer::str189(const std::string &utf8)
{
	varint(static_cast<uint32_t>(utf8.size()));
	raw(reinterpret_cast<const uint8_t *>(utf8.data()), utf8.size());
}

// -- Position / chat / misc -------------------------------------------------

int64_t encodePosition(int32_t x, int32_t y, int32_t z)
{
	const uint64_t ux = static_cast<uint32_t>(x) & 0x3FFFFFFu;
	const uint64_t uy = static_cast<uint32_t>(y) & 0xFFFu;
	const uint64_t uz = static_cast<uint32_t>(z) & 0x3FFFFFFu;
	return static_cast<int64_t>((ux << 38) | (uy << 26) | uz);
}

void decodePosition(int64_t v, int32_t &x, int32_t &y, int32_t &z)
{
	const uint64_t u = static_cast<uint64_t>(v);
	int64_t sx = static_cast<int64_t>((u >> 38) & 0x3FFFFFFu);
	int64_t sy = static_cast<int64_t>((u >> 26) & 0xFFFu);
	int64_t sz = static_cast<int64_t>(u & 0x3FFFFFFu);
	if (sx >= (1 << 25))
		sx -= (1 << 26);
	if (sy >= (1 << 11))
		sy -= (1 << 12);
	if (sz >= (1 << 25))
		sz -= (1 << 26);
	x = static_cast<int32_t>(sx);
	y = static_cast<int32_t>(sy);
	z = static_cast<int32_t>(sz);
}

std::string chatToText(const std::string &json)
{
	AuthJson::Value root;
	if (!AuthJson::parse(json, root))
		return json;
	std::string out;
	chatWalk(root, out);
	return out;
}

std::string sanitizeMotd(const std::string &text)
{
	std::string out;
	const uint8_t *p = reinterpret_cast<const uint8_t *>(text.data());
	std::size_t left = text.size();
	while (left > 0)
	{
		const std::size_t len = utf8PointLen(*p);
		if (len == 0 || len > left)
			break;
		const uint32_t cp = decodeUtf8Point(p, len);
		if (cp != 0xA7 && cp != '\n' && cp != '\r')
		{
			Writer w;
			encodeUtf8(w, cp);
			out.append(w.b.begin(), w.b.end());
		}
		p += len;
		left -= len;
	}
	return out;
}

std::string truncateUtf8(const std::string &text, std::size_t maxCodePoints)
{
	const uint8_t *p = reinterpret_cast<const uint8_t *>(text.data());
	std::size_t left = text.size();
	std::size_t count = 0;
	std::size_t bytes = 0;
	while (left > 0 && count < maxCodePoints)
	{
		const std::size_t len = utf8PointLen(*p);
		if (len == 0 || len > left)
			break;
		p += len;
		left -= len;
		bytes += len;
		count++;
	}
	return text.substr(0, bytes);
}

// -- NBT skip -----------------------------------------------------------------
//
// Returns the length of one TAG_Compound blob (0x00 leading byte = empty,
// length 1). Mirrors proxy/proto189.py's structural skipper: no full NBT
// model, just enough to find the blob end for slot conversion.

namespace
{

bool nbtCompoundEnd(const uint8_t *d, std::size_t len, std::size_t &off);
bool nbtPayloadEnd(const uint8_t *d, std::size_t len, std::size_t &off, uint8_t tag);
bool nbtListItemEnd(const uint8_t *d, std::size_t len, std::size_t &off, uint8_t tag);

bool need(const uint8_t *d, std::size_t len, std::size_t off, std::size_t n)
{
	(void)d;
	return off + n <= len;
}

int32_t peekI32(const uint8_t *d, std::size_t off)
{
	return static_cast<int32_t>((d[off] << 24) | (d[off + 1] << 16) |
	                            (d[off + 2] << 8) | d[off + 3]);
}

bool nbtPayloadEnd(const uint8_t *d, std::size_t len, std::size_t &off, uint8_t tag)
{
	switch (tag)
	{
	case 0:
		return true;
	case 1:
		if (!need(d, len, off, 1))
			return false;
		off += 1;
		return true;
	case 2:
		if (!need(d, len, off, 2))
			return false;
		off += 2;
		return true;
	case 3:
	case 4:
		if (!need(d, len, off, 4))
			return false;
		off += 4;
		return true;
	case 5:
	case 6:
		if (!need(d, len, off, 8))
			return false;
		off += 8;
		return true;
	case 7:
	{
		if (!need(d, len, off, 4))
			return false;
		const int32_t n = peekI32(d, off);
		if (n < 0 || !need(d, len, off, 4 + static_cast<std::size_t>(n)))
			return false;
		off += 4 + static_cast<std::size_t>(n);
		return true;
	}
	case 8:
	{
		if (!need(d, len, off, 2))
			return false;
		const int32_t n = (d[off] << 8) | d[off + 1];
		if (!need(d, len, off, 2 + static_cast<std::size_t>(n)))
			return false;
		off += 2 + static_cast<std::size_t>(n);
		return true;
	}
	case 9:
	{
		if (!need(d, len, off, 5))
			return false;
		const uint8_t etag = d[off];
		const int32_t n = peekI32(d, off + 1);
		off += 5;
		if (n < 0)
			return false;
		for (int32_t i = 0; i < n; i++)
			if (!nbtListItemEnd(d, len, off, etag))
				return false;
		return true;
	}
	case 10:
		return nbtCompoundEnd(d, len, off);
	case 11:
	{
		if (!need(d, len, off, 4))
			return false;
		const int32_t n = peekI32(d, off);
		if (n < 0 || !need(d, len, off, 4 + 4 * static_cast<std::size_t>(n)))
			return false;
		off += 4 + 4 * static_cast<std::size_t>(n);
		return true;
	}
	case 12:
	{
		if (!need(d, len, off, 4))
			return false;
		const int32_t n = peekI32(d, off);
		if (n < 0 || !need(d, len, off, 4 + 8 * static_cast<std::size_t>(n)))
			return false;
		off += 4 + 8 * static_cast<std::size_t>(n);
		return true;
	}
	default:
		return false;
	}
}

bool nbtListItemEnd(const uint8_t *d, std::size_t len, std::size_t &off, uint8_t tag)
{
	switch (tag)
	{
	case 1:
		if (!need(d, len, off, 1))
			return false;
		off += 1;
		return true;
	case 2:
		if (!need(d, len, off, 2))
			return false;
		off += 2;
		return true;
	case 3:
	case 4:
		if (!need(d, len, off, 4))
			return false;
		off += 4;
		return true;
	case 5:
	case 6:
		if (!need(d, len, off, 8))
			return false;
		off += 8;
		return true;
	case 7:
	{
		if (!need(d, len, off, 4))
			return false;
		const int32_t n = peekI32(d, off);
		if (n < 0 || !need(d, len, off, 4 + static_cast<std::size_t>(n)))
			return false;
		off += 4 + static_cast<std::size_t>(n);
		return true;
	}
	case 8:
	{
		if (!need(d, len, off, 2))
			return false;
		const int32_t n = (d[off] << 8) | d[off + 1];
		if (!need(d, len, off, 2 + static_cast<std::size_t>(n)))
			return false;
		off += 2 + static_cast<std::size_t>(n);
		return true;
	}
	case 9:
		return false; // nested lists unsupported (same contract as proxy)
	case 10:
		return nbtCompoundEnd(d, len, off);
	case 11:
	{
		if (!need(d, len, off, 4))
			return false;
		const int32_t n = peekI32(d, off);
		if (n < 0 || !need(d, len, off, 4 + 4 * static_cast<std::size_t>(n)))
			return false;
		off += 4 + 4 * static_cast<std::size_t>(n);
		return true;
	}
	case 12:
	{
		if (!need(d, len, off, 4))
			return false;
		const int32_t n = peekI32(d, off);
		if (n < 0 || !need(d, len, off, 4 + 8 * static_cast<std::size_t>(n)))
			return false;
		off += 4 + 8 * static_cast<std::size_t>(n);
		return true;
	}
	default:
		return false;
	}
}

bool nbtCompoundEnd(const uint8_t *d, std::size_t len, std::size_t &off)
{
	for (;;)
	{
		if (!need(d, len, off, 1))
			return false;
		const uint8_t tag = d[off++];
		if (tag == 0)
			return true;
		if (!need(d, len, off, 2))
			return false;
		const std::size_t nameLen =
			static_cast<std::size_t>((d[off] << 8) | d[off + 1]);
		off += 2;
		if (!need(d, len, off, nameLen))
			return false;
		off += nameLen;
		if (!nbtPayloadEnd(d, len, off, tag))
			return false;
	}
}

} // namespace

std::size_t skipNbt(const uint8_t *p, std::size_t len)
{
	if (len < 1)
		return 0;
	if (p[0] == 0)
		return 1;
	if (p[0] != 10)
		return 0;
	std::size_t off = 1;
	if (len < 3)
		return 0;
	const std::size_t nameLen =
		static_cast<std::size_t>((p[1] << 8) | p[2]);
	off = 3 + nameLen;
	if (off > len)
		return 0;
	if (!nbtCompoundEnd(p, len, off))
		return 0;
	return off;
}

// -- slots --------------------------------------------------------------------

bool convertSlot189To125(Cursor &c, Writer &w)
{
	const int16_t id = c.i16();
	if (!c.ok)
		return false;
	w.i16(id);
	if (id == -1)
		return true;
	const uint8_t count = c.u8();
	const int16_t damage = c.i16();
	if (!c.ok)
		return false;
	// 1.8 NBT: leading 0x00 = none, else a named blob we re-wrap with the
	// 1.2.5 short length prefix (both are uncompressed binary tag data).
	if (c.left < 1)
		return false;
	std::size_t blobLen = 0;
	if (c.p[0] != 0)
	{
		blobLen = skipNbt(c.p, c.left);
		if (blobLen == 0)
			return false;
	}
	else
		blobLen = 1; // hmm: 1.8 empty marker is 1 byte; 1.2.5 wants length 0
	w.u8(count);
	w.i16(damage);
	if (c.p[0] == 0)
	{
		w.i16(0);
		c.skip(1);
	}
	else
	{
		if (blobLen > 32767)
			return false;
		w.i16(static_cast<int16_t>(blobLen));
		w.raw(c.p, blobLen);
		c.skip(blobLen);
	}
	return c.ok;
}

bool convertSlot125To189(Cursor &c, Writer &w)
{
	const int16_t id = c.i16();
	if (!c.ok)
		return false;
	w.i16(id);
	if (id == -1)
		return true;
	const uint8_t count = c.u8();
	const int16_t damage = c.i16();
	const int16_t nbtLen = c.i16();
	if (!c.ok || nbtLen < 0 || static_cast<std::size_t>(nbtLen) > c.left)
		return false;
	w.u8(count);
	w.i16(damage);
	if (nbtLen == 0)
		w.u8(0);
	else
		w.raw(c.p, static_cast<std::size_t>(nbtLen));
	c.skip(static_cast<std::size_t>(nbtLen));
	return c.ok;
}

// -- chunks ---------------------------------------------------------------------

bool convertChunkSections(uint16_t bitmask, const uint8_t *data, std::size_t len,
                          bool skylight, uint16_t &primaryOut, uint16_t &addOut,
                          std::vector<uint8_t> &rawOut)
{
	struct Section
	{
		uint16_t index = 0;
		const uint8_t *ids = nullptr; // 4096 low bytes (borrowed scratch)
	};
	// Decode into scratch: ids[16][4096] + metas packed straight out.
	uint16_t present[16];
	int presentCount = 0;
	std::size_t off = 0;
	static uint16_t scratchIds[16][4096];
	static uint8_t scratchMetas[16][2048];
	for (int sec = 0; sec < 16; sec++)
	{
		if (!((bitmask >> sec) & 1))
			continue;
		if (off + 8192 + 2048 > len)
			return false;
		for (int i = 0; i < 4096; i++)
		{
			const uint16_t v = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
			off += 2;
			scratchIds[presentCount][i] = v >> 4;
			const uint8_t meta = static_cast<uint8_t>(v & 0xF);
			if (i & 1)
				scratchMetas[presentCount][i >> 1] |= static_cast<uint8_t>(meta << 4);
			else
				scratchMetas[presentCount][i >> 1] = meta;
		}
		// Block light: copied verbatim below from the source span; record
		// the offset now (sky handled the same way).
		present[presentCount] = static_cast<uint16_t>(sec);
		presentCount++;
		// NOTE: light offsets are recomputed in the emit pass from the same
		// layout math, so only the section order matters here.
		off += 2048; // block light
		if (skylight)
		{
			if (off + 2048 > len)
				return false;
			off += 2048;
		}
	}
	if (off != len)
		return false;

	// Re-walk for light spans (same order, same math).
	std::size_t lightBase[16];
	off = 0;
	for (int k = 0; k < presentCount; k++)
	{
		off += 8192;
		lightBase[k] = off;
		off += 2048;
		if (skylight)
			off += 2048;
	}

	Writer raw;
	uint16_t add = 0;
	for (int k = 0; k < presentCount; k++)
	{
		for (int i = 0; i < 4096; i++)
			raw.u8(static_cast<uint8_t>(scratchIds[k][i] & 0xFF));
		raw.raw(scratchMetas[k], 2048);
		raw.raw(data + lightBase[k], 2048);
		if (skylight)
			raw.raw(data + lightBase[k] + 2048, 2048);
		else
		{
			for (int i = 0; i < 2048; i++)
				raw.u8(0xFF);
		}
	}
	static uint8_t scratchAdd[2048];
	for (int k = 0; k < presentCount; k++)
	{
		bool anyHigh = false;
		for (int i = 0; i < 4096; i++)
			if (scratchIds[k][i] > 255)
			{
				anyHigh = true;
				break;
			}
		if (!anyHigh)
			continue;
		add |= static_cast<uint16_t>(1u << present[k]);
		for (int i = 0; i < 4096; i++)
		{
			const uint8_t hi = static_cast<uint8_t>((scratchIds[k][i] >> 8) & 0xF);
			if (i & 1)
				scratchAdd[i >> 1] |= static_cast<uint8_t>(hi << 4);
			else
				scratchAdd[i >> 1] = hi;
		}
		raw.raw(scratchAdd, 2048);
	}
	primaryOut = bitmask;
	addOut = add;
	rawOut = std::move(raw.b);
	return true;
}

// -- login open -------------------------------------------------------------------

std::vector<uint8_t> encodeHandshake189(const std::string &host, uint16_t port,
                                        uint32_t nextState)
{
	Writer payload;
	payload.varint(kTargetProtocol);
	payload.str189(host);
	payload.u16(port);
	payload.varint(nextState);
	std::vector<uint8_t> frame;
	framePlain189(0x00, payload.b, frame);
	return frame;
}

std::vector<uint8_t> encodeLoginStart189(const std::string &username)
{
	Writer payload;
	payload.str189(username);
	std::vector<uint8_t> frame;
	framePlain189(0x00, payload.b, frame);
	return frame;
}

// -- serverbound --------------------------------------------------------------------
namespace
{

void emit189(uint32_t id, const Writer &payload,
             std::vector<std::vector<uint8_t>> &framesOut)
{
	std::vector<uint8_t> frame;
	framePlain189(id, payload.b, frame);
	framesOut.push_back(std::move(frame));
}

int stageOf(Session189 &s)
{
	return s.stage.load(std::memory_order_acquire);
}

} // namespace

bool translateServerbound(Session189 &s, uint8_t nativeId,
                          const uint8_t *payload, std::size_t len,
                          std::vector<std::vector<uint8_t>> &framesOut)
{
	const int stage = stageOf(s);
	Cursor c(payload, len);
	if (nativeId == kNHandshake && stage == static_cast<int>(Session189::Stage::Idle))
	{
		// The game always opens with Packet2Handshake(username): answer on
		// the 1.8 side instead and hold the login state here. Nothing is
		// forwarded as 1.2.5.
		s.username = c.str125(64);
		if (!c.ok || c.left != 0)
			return false;
		s.stage.store(static_cast<int>(Session189::Stage::Login),
		              std::memory_order_release);
		return true; // frames emitted by the IO layer (needs host/port)
	}
	if (stage != static_cast<int>(Session189::Stage::Play))
	{
		// Login swallowed (the handler sends Packet1Login after our
		// synthesized "-" handshake; the 1.8 login already happened), quit
		// dropped (the socket close follows), anything else unexpected.
		return nativeId == kNLogin || nativeId == kNKick;
	}
	switch (nativeId)
	{
	case kNKeepAlive:
	{
		Writer w;
		w.varint(static_cast<uint32_t>(c.i32()));
		if (!c.ok || c.left != 0)
			return false;
		emit189(0x00, w, framesOut);
		return true;
	}
	case kNChat:
	{
		const std::string text = c.str125(119);
		if (!c.ok || c.left != 0)
			return false;
		Writer w;
		w.str189(truncateUtf8(text, 100)); // 1.8 chat limit
		emit189(0x01, w, framesOut);
		return true;
	}
	case kNFlying:
	{
		Writer w;
		w.u8(c.u8());
		if (!c.ok || c.left != 0)
			return false;
		emit189(0x03, w, framesOut);
		return true;
	}
	case kNPos:
	{
		const double x = c.f64(), y = c.f64();
		c.f64(); // stance: 1.8 has none
		const double z = c.f64();
		const uint8_t ground = c.u8();
		if (!c.ok || c.left != 0)
			return false;
		{
			std::lock_guard<std::mutex> guard(s.posMutex);
			s.posX = x;
			s.posY = y;
			s.posZ = z;
		}
		Writer w;
		w.f64(x);
		w.f64(y);
		w.f64(z);
		w.u8(ground);
		emit189(0x04, w, framesOut);
		return true;
	}
	case kNLook:
	{
		const float yaw = c.f32(), pitch = c.f32();
		const uint8_t ground = c.u8();
		if (!c.ok || c.left != 0)
			return false;
		{
			std::lock_guard<std::mutex> guard(s.posMutex);
			s.yaw = yaw;
			s.pitch = pitch;
		}
		Writer w;
		w.f32(yaw);
		w.f32(pitch);
		w.u8(ground);
		emit189(0x05, w, framesOut);
		return true;
	}
	case kNPosLook:
	{
		const double x = c.f64(), y = c.f64();
		c.f64(); // stance
		const double z = c.f64();
		const float yaw = c.f32(), pitch = c.f32();
		const uint8_t ground = c.u8();
		if (!c.ok || c.left != 0)
			return false;
		{
			std::lock_guard<std::mutex> guard(s.posMutex);
			s.posX = x;
			s.posY = y;
			s.posZ = z;
			s.yaw = yaw;
			s.pitch = pitch;
		}
		Writer w;
		w.f64(x);
		w.f64(y);
		w.f64(z);
		w.f32(yaw);
		w.f32(pitch);
		w.u8(ground);
		emit189(0x06, w, framesOut);
		return true;
	}
	case kNDig:
	{
		const uint8_t status = c.u8();
		const int32_t x = c.i32();
		const uint8_t y = c.u8();
		const int32_t z = c.i32();
		const int8_t face = c.i8();
		if (!c.ok || c.left != 0)
			return false;
		Writer out;
		out.varint(status);
		out.i64(encodePosition(x, static_cast<int32_t>(y), z));
		out.i8(face);
		emit189(0x07, out, framesOut);
		return true;
	}
	case kNPlace:
	{
		const int32_t x = c.i32();
		const uint8_t y = c.u8();
		const int32_t z = c.i32();
		const int8_t dir = c.i8();
		Writer slot;
		if (!convertSlot125To189(c, slot) || c.left != 0)
			return false;
		Writer out;
		out.i64(encodePosition(x, static_cast<int32_t>(y), z));
		out.i8(dir);
		out.raw(slot.b);
		out.u8(8);
		out.u8(8);
		out.u8(8); // cursor: block center
		emit189(0x08, out, framesOut);
		return true;
	}
	case kNHeld:
	{
		Writer w;
		w.i16(c.i16());
		if (!c.ok || c.left != 0)
			return false;
		emit189(0x09, w, framesOut);
		return true;
	}
	case kNAnimation:
	{
		c.i32();
		c.u8();
		if (!c.ok || c.left != 0)
			return false;
		Writer w; // 1.8 Arm Swing carries no payload
		emit189(0x0A, w, framesOut);
		return true;
	}
	case kNEntityAction:
	{
		c.i32(); // own eid comes from the login state instead
		const uint8_t action = c.u8();
		if (!c.ok || c.left != 0)
			return false;
		uint32_t mapped = 0;
		if (!mapEntityAction(action, mapped))
			return true; // unknown action: drop, not an error
		Writer w;
		w.varint(static_cast<uint32_t>(s.entityId.load(std::memory_order_acquire)));
		w.varint(mapped);
		w.varint(0);
		emit189(0x0B, w, framesOut);
		return true;
	}
	case kNUseEntity:
	{
		c.i32(); // attacker: always us
		const int32_t target = c.i32();
		const uint8_t left = c.u8();
		if (!c.ok || c.left != 0)
			return false;
		Writer w;
		w.varint(static_cast<uint32_t>(target));
		w.varint(left ? 1u : 0u);
		emit189(0x02, w, framesOut);
		return true;
	}
	case kNRespawn:
	{
		// Respawn request (EntityClientPlayerMP sends Packet9 C->S).
		Writer w;
		w.varint(0); // Client Status: perform respawn
		emit189(0x16, w, framesOut);
		return true;
	}
	case kNLogin:
		return true;  // swallowed (see stage gate above)
	case kNKick:
		return true; // quit: socket close follows, nothing to forward
	default:
		return true; // unmapped native packet: drop, not an error
	}
}

// -- clientbound ----------------------------------------------------------------------
namespace
{

void pushNative(ClientboundOut &out, uint8_t id, const Writer &payload)
{
	NativePacket p;
	p.id = id;
	p.payload = payload.b;
	out.packets.push_back(std::move(p));
}

void pushKick(ClientboundOut &out, const std::string &reason)
{
	Writer w;
	w.str125(reason, 256);
	pushNative(out, kNKick, w);
}

} // namespace

void translateClientbound(Session189 &s, uint32_t id189,
                          const uint8_t *payload, std::size_t len,
                          ClientboundOut &out)
{
	const int stage = stageOf(s);
	Cursor c(payload, len);
	if (stage == static_cast<int>(Session189::Stage::Login))
	{
		switch (id189)
		{
		case kLoginDisconnect:
		{
			pushKick(out, chatToText(c.str189()));
			return;
		}
		case kEncryptionRequest:
		{
			pushKick(out, "Online-mode servers need the phase-2 encryption "
			              "adapter; use an offline-mode 1.8.9 server.");
			return;
		}
		case kSetCompression:
		{
			s.compressionThreshold.store(c.svarint(), std::memory_order_release);
			return;
		}
		case kLoginSuccess:
		{
			c.str189(); // uuid: the engine has no uuid field to fill
			c.str189(); // profile name
			Writer w;
			w.str125("-", 64); // offline-style key: handler sends Packet1Login
			pushNative(out, kNHandshake, w);
			s.stage.store(static_cast<int>(Session189::Stage::Play),
			              std::memory_order_release);
			return;
		}
		default:
			return; // ignore anything else during login
		}
	}
	if (stage != static_cast<int>(Session189::Stage::Play))
		return;
	switch (id189)
	{
	case kKeepAlive:
	{
		Writer w;
		w.i32(static_cast<int32_t>(c.varint()));
		pushNative(out, kNKeepAlive, w);
		return;
	}
	case kJoinGame:
	{
		const int32_t eid = c.i32();
		const uint8_t mode = static_cast<uint8_t>(c.u8() & 0x07);
		const int8_t dim = c.i8();
		const uint8_t difficulty = c.u8();
		const uint8_t maxPlayers = c.u8();
		const std::string level = c.str189();
		if (!c.ok)
			return;
		s.entityId.store(eid, std::memory_order_release);
		s.dimension.store(dim, std::memory_order_release);
		Writer w;
		w.i32(eid);
		w.str125(level.empty() ? "default" : level, 16);
		w.i32(mode); // native S->C layout carries ints (Packet1Login)
		w.i32(dim);
		w.u8(difficulty);
		w.u8(0);
		w.u8(maxPlayers);
		pushNative(out, kNLogin, w);
		return;
	}
	case kChat:
	{
		const std::string text = chatToText(c.str189());
		c.u8(); // position: chat/system/actionbar all read as chat
		if (!c.ok)
			return;
		Writer w;
		w.str125(text, 119);
		pushNative(out, kNChat, w);
		return;
	}
	case kTimeUpdate:
	{
		c.i64(); // world age: no native field for it
		const int64_t time = c.i64();
		if (!c.ok)
			return;
		Writer w;
		w.i64(time);
		pushNative(out, kNTime, w);
		return;
	}
	case kSpawnPosition:
	{
		int32_t x, y, z;
		decodePosition(c.i64(), x, y, z);
		if (!c.ok)
			return;
		Writer w;
		w.i32(x);
		w.i32(y);
		w.i32(z);
		pushNative(out, kNSpawnPos, w);
		return;
	}
	case kUpdateHealth:
	{
		float hp = c.f32();
		const uint32_t food = c.varint();
		const float sat = c.f32();
		if (!c.ok)
			return;
		if (hp < 0)
			hp = 0;
		if (hp > 20)
			hp = 20;
		Writer w;
		w.i16(static_cast<int16_t>(hp));
		w.i16(static_cast<int16_t>(food > 20 ? 20 : food));
		w.f32(sat);
		pushNative(out, kNHealth, w);
		return;
	}
	case kRespawn:
	{
		const int32_t dim = c.i32();
		const uint8_t difficulty = c.u8();
		const uint8_t mode = static_cast<uint8_t>(c.u8() & 0x07);
		const std::string level = c.str189();
		if (!c.ok)
			return;
		s.dimension.store(dim, std::memory_order_release);
		Writer w;
		w.i8(static_cast<int8_t>(dim));
		w.u8(difficulty);
		w.u8(mode);
		w.i16(256);
		w.str125(level.empty() ? "default" : level, 16);
		pushNative(out, kNRespawn, w);
		return;
	}
	case kPlayerPosLook:
	{
		double x = c.f64(), y = c.f64(), z = c.f64();
		float yaw = c.f32(), pitch = c.f32();
		const uint8_t flags = c.u8();
		if (!c.ok)
			return;
		{
			std::lock_guard<std::mutex> guard(s.posMutex);
			if (flags & 0x01)
				x += s.posX;
			if (flags & 0x02)
				y += s.posY;
			if (flags & 0x04)
				z += s.posZ;
			if (flags & 0x08)
				yaw += s.yaw;
			if (flags & 0x10)
				pitch += s.pitch;
			s.posX = x;
			s.posY = y;
			s.posZ = z;
			s.yaw = yaw;
			s.pitch = pitch;
		}
		Writer w;
		w.f64(x);
		w.f64(y);
		w.f64(y + 1.62);
		w.f64(z);
		w.f32(yaw);
		w.f32(pitch);
		w.u8(0);
		pushNative(out, kNPosLook, w);
		return;
	}
	case kAnimation:
	{
		Writer w;
		w.i32(static_cast<int32_t>(c.varint()));
		w.u8(c.u8());
		if (!c.ok)
			return;
		pushNative(out, kNAnimation, w);
		return;
	}
	case kChunkData:
	{
		const int32_t cx = c.i32(), cz = c.i32();
		const bool groundUp = c.u8() != 0;
		const uint16_t bitmask = c.u16();
		const uint32_t size = c.varint();
		if (!c.ok || size > c.left)
			return;
		const uint8_t *data = c.p;
		const bool skylight = s.dimension.load(std::memory_order_acquire) == 0;
		if (bitmask == 0)
		{
			Writer w;
			w.i32(cx);
			w.i32(cz);
			w.u8(0);
			pushNative(out, kNPreChunk, w);
			return;
		}
		// 1.8 appends the 256 biome bytes after the section data when
		// groundUp is set (the selftest-caught split).
		const uint8_t *sections = data;
		std::size_t sectionsLen = size;
		const uint8_t *biome = nullptr;
		if (groundUp)
		{
			if (size < 256)
				return;
			sectionsLen = size - 256;
			biome = data + sectionsLen;
		}
		uint16_t primary = 0, add = 0;
		std::vector<uint8_t> raw;
		if (!convertChunkSections(bitmask, sections, sectionsLen, skylight,
		                          primary, add, raw))
			return;
		out.hasChunk = true;
		out.chunk.x = cx;
		out.chunk.z = cz;
		out.chunk.groundUp = groundUp;
		out.chunk.primary = primary;
		out.chunk.add = add;
		out.chunk.raw = std::move(raw);
		if (groundUp && biome != nullptr)
			out.chunk.biome.assign(biome, biome + 256);
		{
			Writer w;
			w.i32(cx);
			w.i32(cz);
			w.u8(1);
			pushNative(out, kNPreChunk, w);
		}
		return;
	}
	case kMultiBlockChange:
	{
		const int32_t cx = c.i32(), cz = c.i32();
		const uint32_t count = c.varint();
		if (!c.ok || count > 4096)
			return;
		Writer w;
		w.i32(cx);
		w.i32(cz);
		w.i16(static_cast<int16_t>(count));
		w.i32(static_cast<int32_t>(count * 4));
		for (uint32_t i = 0; i < count; i++)
		{
			const uint32_t v = c.varint();
			if (!c.ok)
				return;
			const uint8_t lx = static_cast<uint8_t>((v >> 28) & 0xF);
			const uint8_t lz = static_cast<uint8_t>((v >> 24) & 0xF);
			const uint8_t ly = static_cast<uint8_t>((v >> 12) & 0xFF);
			const uint8_t bid = static_cast<uint8_t>(((v >> 4) & 0xFFF) & 0xFF);
			const uint8_t meta = static_cast<uint8_t>(v & 0xF);
			w.i16(static_cast<int16_t>((lx << 12) | (lz << 8) | ly));
			w.u8(bid);
			w.u8(meta);
		}
		pushNative(out, kNMultiBlock, w);
		return;
	}
	case kBlockChange:
	{
		int32_t x, y, z;
		decodePosition(c.i64(), x, y, z);
		const uint32_t state = c.varint();
		if (!c.ok)
			return;
		Writer w;
		w.i32(x);
		w.u8(static_cast<uint8_t>(y & 0xFF));
		w.i32(z);
		w.u8(static_cast<uint8_t>(((state >> 4) & 0xFFF) & 0xFF));
		w.u8(static_cast<uint8_t>(state & 0xF));
		pushNative(out, kNBlockChange, w);
		return;
	}
	case kPlayerList:
	{
		const uint32_t action = c.varint();
		const uint32_t count = c.varint();
		if (!c.ok || count > 1024)
			return;
		for (uint32_t i = 0; i < count; i++)
		{
			if (action == 0) // ADD_PLAYER
			{
				c.skip(16); // uuid
				const std::string name = c.str189();
				const uint32_t nprops = c.varint();
				if (!c.ok)
					return;
				for (uint32_t p = 0; p < nprops; p++)
				{
					c.str189();
					c.str189();
					if (!c.ok)
						return;
					if (c.u8() && c.ok)
						c.str189(); // signature
					if (!c.ok)
						return;
				}
				c.varint(); // gamemode
				const uint32_t ping = c.varint();
				if (!c.ok)
					return;
				if (c.u8() && c.ok)
					c.str189(); // display name
				if (!c.ok)
					return;
				Writer w;
				w.str125(name, 16);
				w.u8(1);
				w.i16(static_cast<int16_t>(ping > 32767 ? 32767 : ping));
				pushNative(out, kNPlayerList, w);
			}
			else if (action == 1) // UPDATE_GAMEMODE
			{
				c.skip(16);
				c.varint();
				if (!c.ok)
					return;
			}
			else if (action == 2) // UPDATE_LATENCY
			{
				c.skip(16);
				c.varint();
				if (!c.ok)
					return;
			}
			else if (action == 3) // UPDATE_DISPLAY_NAME
			{
				c.skip(16);
				if (!c.ok)
					return;
				if (c.u8() && c.ok)
					c.str189();
				if (!c.ok)
					return;
			}
			else if (action == 4) // REMOVE_PLAYER
			{
				c.skip(16);
				if (!c.ok)
					return;
				// MVP keeps no uuid->name map (same contract as the proxy):
				// retire a placeholder row so the tab list does not grow
				// without bound.
				Writer w;
				w.str125("?", 16);
				w.u8(0);
				w.i16(0);
				pushNative(out, kNPlayerList, w);
			}
			else
				return; // unknown action: cannot stay in sync, drop rest
		}
		return;
	}
	case kPluginMessage:
	{
		const std::string channel = c.str189();
		if (!c.ok || channel.size() > 16)
			return; // native channel cap is 16 chars
		Writer w;
		w.str125(channel, 16);
		if (c.left > 32766)
			return;
		w.i16(static_cast<int16_t>(c.left));
		w.raw(c.p, c.left);
		pushNative(out, kNPlugin, w);
		return;
	}
	case kDisconnect:
	{
		pushKick(out, chatToText(c.str189()));
		return;
	}
	default:
		return; // phase-2/cosmetic/unknown: framing consumed it already
	}
}

// -- status -----------------------------------------------------------------------

namespace
{

void dumpNode(const AuthJson::Value &node, std::string &out)
{
	switch (node.type)
	{
	case AuthJson::Value::Type::Null:
		out += "null";
		return;
	case AuthJson::Value::Type::Bool:
		out += node.boolean ? "true" : "false";
		return;
	case AuthJson::Value::Type::Number:
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.17g", node.number);
		out += buf;
		return;
	}
	case AuthJson::Value::Type::String:
		out += '"';
		out += AuthJson::escape(node.text);
		out += '"';
		return;
	case AuthJson::Value::Type::Array:
		out += '[';
		for (std::size_t i = 0; i < node.array.size(); i++)
		{
			if (i)
				out += ',';
			dumpNode(node.array[i], out);
		}
		out += ']';
		return;
	case AuthJson::Value::Type::Object:
		out += '{';
		for (std::size_t i = 0; i < node.object.size(); i++)
		{
			if (i)
				out += ',';
			out += '"';
			out += AuthJson::escape(node.object[i].first);
			out += "\":";
			dumpNode(node.object[i].second, out);
		}
		out += '}';
		return;
	}
}

} // namespace

bool statusTo125(const std::string &statusJson, std::string &motdOut,
                 int &onlineOut, int &maxOut)
{
	AuthJson::Value root;
	if (!AuthJson::parse(statusJson, root))
		return false;
	motdOut.clear();
	if (const AuthJson::Value *desc = root.member("description"))
	{
		if (desc->type == AuthJson::Value::Type::String)
			motdOut = desc->text;
		else if (desc->type == AuthJson::Value::Type::Object)
		{
			std::string rebuilt;
			dumpNode(*desc, rebuilt);
			motdOut = chatToText(rebuilt);
		}
	}
	motdOut = sanitizeMotd(motdOut);
	if (motdOut.size() > 60)
		motdOut.resize(60);
	onlineOut = 0;
	maxOut = 0;
	if (const AuthJson::Value *players = root.member("players"))
	{
		if (const AuthJson::Value *online = players->member("online"))
		{
			long long v = 0;
			if (online->numberAsInteger(v))
				onlineOut = static_cast<int>(v);
		}
		if (const AuthJson::Value *max = players->member("max"))
		{
			long long v = 0;
			if (max->numberAsInteger(v))
				maxOut = static_cast<int>(v);
		}
	}
	return true;
}

} // namespace translator189
