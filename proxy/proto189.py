"""1.8.9 (protocol 47, Netty) framing.

Wire: VarInt length + VarInt packet id + payload. After SetCompression,
each packet is VarInt total-length + VarInt data-length (0 = raw) + data.
Unknown play packets can be skipped by length, so the bridge only parses
what it translates.
"""

import socket
import zlib

from .codec import BufReader, BufWriter, decode_position_189, encode_position_189

PROTOCOL_189 = 47


def read_slot189(r):
    """Slot: short id (-1 empty), byte count, short damage, NBT blob."""
    item_id = r.i16()
    if item_id == -1:
        return (-1, 0, 0, b"")
    count = r.u8()
    damage = r.i16()
    nbt = read_nbt189(r)
    return (item_id, count, damage, nbt)


def write_slot189(w, slot):
    item_id, count, damage, nbt = slot
    w.i16(item_id)
    if item_id == -1:
        return
    w.u8(count)
    w.i16(damage)
    write_nbt189(w, nbt)


def read_nbt189(r):
    """1.8 NBT: a leading zero byte means 'no NBT'; otherwise parse TAG
    lengths structurally to find the blob end (no full NBT decode)."""
    at = r._o
    if r.u8() == 0:
        return b""
    end = _nbt_skip(r._d, r._o - 1)
    blob = r._d[at:end]
    r._o = end
    return blob


def _nbt_skip(data, off):
    """Return the offset just past the TAG_Compound starting at off."""
    # tag id + name length + name, then payload
    if data[off] != 10:
        raise ValueError("expected TAG_Compound")
    name_len = int.from_bytes(data[off + 1:off + 3], "big")
    return _nbt_compound_end(data, off + 3 + name_len)


def _nbt_payload_end(data, off, tag):
    if tag == 0:
        return off  # TAG_End: no payload
    if tag == 1:
        return off + 1
    if tag == 2:
        return off + 2
    if tag in (3, 4):
        return off + 4
    if tag in (5, 6):
        return off + 8
    if tag == 7:
        n = int.from_bytes(data[off:off + 4], "big", signed=True)
        return off + 4 + max(n, 0)
    if tag == 8:
        n = int.from_bytes(data[off:off + 2], "big", signed=True)
        return off + 2 + max(n, 0)
    if tag == 9:
        etag = data[off]
        n = int.from_bytes(data[off + 1:off + 5], "big", signed=True)
        p = off + 5
        if etag == 0 and n == 0:
            return p
        for _ in range(max(n, 0)):
            p = _nbt_list_item_end(data, p, etag)
        return p
    if tag == 10:
        return _nbt_compound_end(data, off)
    if tag == 11:
        n = int.from_bytes(data[off:off + 4], "big", signed=True)
        return off + 4 + 4 * max(n, 0)
    if tag == 12:
        n = int.from_bytes(data[off:off + 4], "big", signed=True)
        return off + 4 + 8 * max(n, 0)
    raise ValueError("bad NBT tag %d" % tag)


def _nbt_list_item_end(data, off, etag):
    if etag in (1,):
        return off + 1
    if etag in (2,):
        return off + 2
    if etag in (3, 4):
        return off + 4
    if etag in (5, 6):
        return off + 8
    if etag == 7:
        n = int.from_bytes(data[off:off + 4], "big", signed=True)
        return off + 4 + max(n, 0)
    if etag == 8:
        n = int.from_bytes(data[off:off + 2], "big", signed=True)
        return off + 2 + max(n, 0)
    if etag == 9:
        raise ValueError("nested NBT lists unsupported")
    if etag == 10:
        return _nbt_compound_end(data, off)
    if etag == 11:
        n = int.from_bytes(data[off:off + 4], "big", signed=True)
        return off + 4 + 4 * max(n, 0)
    if etag == 12:
        n = int.from_bytes(data[off:off + 4], "big", signed=True)
        return off + 4 + 8 * max(n, 0)
    raise ValueError("bad NBT list tag %d" % etag)


def _nbt_compound_end(data, off):
    while True:
        tag = data[off]
        off += 1
        if tag == 0:
            return off
        name_len = int.from_bytes(data[off:off + 2], "big", signed=True)
        off += 2 + max(name_len, 0)
        off = _nbt_payload_end(data, off, tag)


def write_nbt189(w, blob):
    if not blob:
        w.u8(0)
    else:
        w.raw(blob)


def slot125_to_189(slot):
    """1.2.5 slot (id, count, damage, NBT with short length) -> 1.8 slot
    (same tuple shape; NBT re-wrapped with presence flag)."""
    item_id, count, damage, nbt = slot
    if item_id == -1:
        return (-1, 0, 0, b"")
    if nbt[:1] == b"\x00" or not nbt:
        nbt189 = b""
    else:
        nbt189 = nbt  # both are uncompressed NAMED binary tag blobs
    return (item_id, count, damage, nbt189)


def slot189_to_125(slot):
    item_id, count, damage, nbt = slot
    if item_id == -1:
        return (-1, 0, 0, b"")
    return (item_id, count, damage, nbt)


class Connection189:
    """Blocking 1.8.9 endpoint with optional compression (server side)."""

    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(120)
        self.threshold = -1  # compression off

    def _readn(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("peer closed")
            buf += chunk
        return bytes(buf)

    def _read_varint_sock(self):
        value = 0
        for i in range(5):
            b = self._readn(1)[0]
            value |= (b & 0x7F) << (7 * i)
            if not b & 0x80:
                return value
        raise ValueError("varint too long")

    def recv(self):
        """Returns (packet_id, BufReader over payload)."""
        total = self._read_varint_sock()
        data = self._readn(total)
        if self.threshold >= 0:
            r = BufReader(data)
            dlen = r.uvarint()
            if dlen == 0:
                payload = data[r._o:]
            else:
                payload = zlib.decompress(data[r._o:])
                if len(payload) != dlen:
                    raise ValueError("bad compressed length")
        else:
            payload = data
        r = BufReader(payload)
        return r.uvarint(), r

    def send(self, packet_id, payload):
        body = BufWriter().varint(packet_id).raw(payload).bytes()
        if self.threshold >= 0 and len(body) >= self.threshold:
            comp = zlib.compress(body)
            out = BufWriter().varint(len(comp) + _varint_len(len(body)))
            out.varint(len(body)).raw(comp)
            self.sock.sendall(_prefix(out.bytes()))
        elif self.threshold >= 0:
            out = BufWriter().varint(len(body) + 1)
            out.varint(0).raw(body)
            self.sock.sendall(_prefix(out.bytes()))
        else:
            self.sock.sendall(_prefix(body))

    def send_raw(self, packet_id, writer):
        self.send(packet_id, writer.bytes())

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def _varint_len(v):
    v &= 0xFFFFFFFF
    n = 0
    while True:
        n += 1
        v >>= 7
        if not v:
            return n


def _prefix(body):
    return BufWriter().varint(len(body)).raw(body).bytes()


# -- login helpers ------------------------------------------------------

def send_handshake(conn, host, port, next_state):
    w = BufWriter()
    w.varint(PROTOCOL_189).str189(host).u16(port).varint(next_state)
    conn.send(0x00, w.bytes())


def do_status(conn):
    """Status phase (caller already sent handshake next_state=1).
    Returns (json_string, ping_ms)."""
    conn.send(0x00, b"")
    pid, r = conn.recv()
    if pid != 0x00:
        raise ValueError("expected status response, got 0x%02X" % pid)
    js = r.str189()
    import time
    w = BufWriter()
    now_ms = int(time.time() * 1000)
    w.i64(now_ms)
    conn.send(0x01, w.bytes())
    pid, r = conn.recv()
    if pid != 0x01:
        raise ValueError("expected status pong, got 0x%02X" % pid)
    sent = r.i64()
    return js, max(0, int(time.time() * 1000) - sent)


class OnlineModeNeeded(Exception):
    pass


def do_login(conn, username):
    """Login phase (caller already sent handshake next_state=2).
    Returns (uuid_string, username). Offline servers only for now."""
    w = BufWriter()
    w.str189(username)
    conn.send(0x00, w.bytes())
    while True:
        pid, r = conn.recv()
        if pid == 0x00:  # Disconnect
            from .codec import chat_to_text
            raise ConnectionError("server refused login: " + chat_to_text(r.str189()))
        if pid == 0x01:  # Encryption Request -> online mode
            raise OnlineModeNeeded(
                "this server runs in online mode (encryption); "
                "the MVP bridge supports offline-mode 1.8.9 servers only")
        if pid == 0x03:  # Set Compression
            conn.threshold = r.varint()
            continue
        if pid == 0x02:  # Login Success
            uuid = r.str189()
            name = r.str189()
            return uuid, name
        raise ValueError("unexpected login packet 0x%02X" % pid)
