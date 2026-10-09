"""Shared primitives for both wire protocols.

1.2.5 (protocol 29, pre-Netty): big-endian fixed fields, strings are
short char-count + UTF-16BE, bools are one byte.
1.8.9 (protocol 47, Netty): VarInt-prefixed framing, strings are VarInt
byte-count + UTF-8.
"""

import json
import struct


class BufReader:
    def __init__(self, data):
        self._d = data
        self._o = 0

    def remaining(self):
        return len(self._d) - self._o

    def read(self, n):
        if self._o + n > len(self._d):
            raise ValueError("truncated buffer")
        out = self._d[self._o:self._o + n]
        self._o += n
        return out

    def u8(self):
        return self.read(1)[0]

    def i8(self):
        v = self.u8()
        return v - 256 if v >= 128 else v

    def u16(self):
        return struct.unpack(">H", self.read(2))[0]

    def i16(self):
        return struct.unpack(">h", self.read(2))[0]

    def i32(self):
        return struct.unpack(">i", self.read(4))[0]

    def i64(self):
        return struct.unpack(">q", self.read(8))[0]

    def f32(self):
        return struct.unpack(">f", self.read(4))[0]

    def f64(self):
        return struct.unpack(">d", self.read(8))[0]

    def bool(self):
        return self.u8() != 0

    def varint(self):
        value = 0
        for i in range(5):
            b = self.u8()
            value |= (b & 0x7F) << (7 * i)
            if not b & 0x80:
                if value >= 1 << 31:
                    value -= 1 << 32
                return value
        raise ValueError("varint too long")

    def uvarint(self):
        value = 0
        for i in range(5):
            b = self.u8()
            value |= (b & 0x7F) << (7 * i)
            if not b & 0x80:
                return value
        raise ValueError("varint too long")

    def uuid(self):
        return self.read(16)

    # -- protocol-specific strings --------------------------------------
    def str125(self):
        """1.2.5 string: short char count + UTF-16BE."""
        n = self.i16()
        if n < 0:
            raise ValueError("negative string length")
        return self.read(n * 2).decode("utf-16-be")

    def str189(self):
        """1.8 string: VarInt byte count + UTF-8."""
        n = self.uvarint()
        return self.read(n).decode("utf-8", "replace")


class BufWriter:
    def __init__(self):
        self._b = bytearray()

    def bytes(self):
        return bytes(self._b)

    def u8(self, v):
        self._b.append(v & 0xFF)
        return self

    def i8(self, v):
        return self.u8(v)

    def u16(self, v):
        self._b += struct.pack(">H", v & 0xFFFF)
        return self

    def i16(self, v):
        self._b += struct.pack(">h", v)
        return self

    def i32(self, v):
        self._b += struct.pack(">i", v)
        return self

    def i64(self, v):
        self._b += struct.pack(">q", v)
        return self

    def f32(self, v):
        self._b += struct.pack(">f", v)
        return self

    def f64(self, v):
        self._b += struct.pack(">d", v)
        return self

    def bool(self, v):
        return self.u8(1 if v else 0)

    def varint(self, v):
        v &= 0xFFFFFFFF
        while True:
            b = v & 0x7F
            v >>= 7
            if v:
                self._b.append(b | 0x80)
            else:
                self._b.append(b)
                return self

    def raw(self, data):
        self._b += data
        return self

    def str125(self, s):
        enc = s.encode("utf-16-be")
        self.i16(len(s))
        self._b += enc
        return self

    def str189(self, s):
        enc = s.encode("utf-8")
        self.varint(len(enc))
        self._b += enc
        return self


def encode_position_189(x, y, z):
    """1.8 64-bit Position: x(26) | y(12) | z(26), two's complement."""
    return (((x & 0x3FFFFFF) << 38) | ((y & 0xFFF) << 26) | (z & 0x3FFFFFF))


def decode_position_189(value):
    x = (value >> 38) & 0x3FFFFFF
    y = (value >> 26) & 0xFFF
    z = value & 0x3FFFFFF
    if x >= 1 << 25:
        x -= 1 << 26
    if z >= 1 << 25:
        z -= 1 << 26
    if y >= 1 << 11:
        y -= 1 << 12
    return x, y, z


def chat_to_text(chat):
    """1.8 JSON chat -> plain text with legacy section codes kept."""
    try:
        node = json.loads(chat)
    except (ValueError, TypeError):
        return chat
    parts = []

    def walk(n):
        if isinstance(n, str):
            parts.append(n)
            return
        if not isinstance(n, dict):
            return
        parts.append(n.get("text", ""))
        for extra in n.get("extra", []) or []:
            walk(extra)

    walk(node)
    return "".join(parts)


def strip_section_codes(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\u00a7" and i + 1 < len(s):
            i += 2
            continue
        out.append(s[i])
        i += 1
    return "".join(out)
