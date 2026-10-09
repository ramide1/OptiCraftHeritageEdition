"""1.2.5 (protocol 29, pre-Netty) framing and the packet subset the bridge
speaks. Fixed-size packets: every id has a known layout, so encode/decode
is a field list. Fields: b=signed byte, B=unsigned byte, h=short, i=int,
q=long, f=float, d=double, s=string, ?=bool."""

import socket

from .codec import BufReader, BufWriter


# id -> (name, [(field, kind), ...]); 'r' = raw bytes with int length prefix
# handled manually (MapChunk, MultiBlockChange, Slot).
PACKETS = {
    0x00: ("KeepAlive", [("id", "i")]),
    0x01: ("Login", [("eid", "i"), ("level", "s"), ("mode", "b"),
                     ("dim", "b"), ("difficulty", "b"), ("unused", "b"),
                     ("maxplayers", "B")]),
    0x02: ("Handshake", [("data", "s")]),
    0x03: ("Chat", [("text", "s")]),
    0x04: ("Time", [("time", "q")]),
    0x06: ("SpawnPos", [("x", "i"), ("y", "i"), ("z", "i")]),
    0x07: ("UseEntity", [("user", "i"), ("target", "i"), ("left", "?")]),
    0x08: ("Health", [("hp", "h"), ("food", "h"), ("sat", "f")]),
    0x09: ("Respawn", [("dim", "b"), ("difficulty", "b"), ("mode", "b"),
                       ("height", "h"), ("level", "s")]),
    0x0A: ("Flying", [("ground", "?")]),
    0x0B: ("Pos", [("x", "d"), ("y", "d"), ("stance", "d"), ("z", "d"),
                   ("ground", "?")]),
    0x0C: ("Look", [("yaw", "f"), ("pitch", "f"), ("ground", "?")]),
    0x0D: ("PosLook", [("x", "d"), ("y", "d"), ("stance", "d"), ("z", "d"),
                       ("yaw", "f"), ("pitch", "f"), ("ground", "?")]),
    0x0E: ("Dig", [("status", "b"), ("x", "i"), ("y", "B"), ("z", "i"),
                   ("face", "b")]),
    0x0F: ("Place", [("x", "i"), ("y", "B"), ("z", "i"), ("dir", "b")]),
    0x10: ("Held", [("slot", "h")]),
    0x12: ("Animation", [("eid", "i"), ("anim", "b")]),
    0x13: ("EntityAction", [("eid", "i"), ("action", "b")]),
    0x32: ("PreChunk", [("x", "i"), ("z", "i"), ("load", "?")]),
    0x33: ("MapChunk", [("x", "i"), ("z", "i"), ("groundup", "?"),
                        ("primary", "h"), ("add", "h")]),
    0x35: ("BlockChange", [("x", "i"), ("y", "B"), ("z", "i"),
                           ("type", "B"), ("meta", "B")]),
    0xC9: ("PlayerList", [("name", "s"), ("online", "?"), ("ping", "h")]),
    0xCD: ("ClientStatus", [("payload", "b")]),
    0xFE: ("ServerPing", []),
    0xFF: ("Kick", [("reason", "s")]),
}

# Expected payload bytes after the id byte (MapChunk/Place/Slot excluded:
# variable length, handled manually).
FIXED_SIZES = {
    0x00: 4, 0x02: None, 0x03: None, 0x04: 8, 0x06: 12, 0x07: 9,
    0x08: 8, 0x0A: 1, 0x0B: 33, 0x0C: 9, 0x0D: 41, 0x0E: 11, 0x10: 2,
    0x12: 5, 0x13: 5, 0x32: 9, 0x35: 11, 0xCD: 1, 0xFE: 0,
}


def read_slot125(r):
    """ItemStack: short id (-1 empty), byte count, short damage,
    [short NBT length + NBT]. Returns (id, count, damage, nbt_bytes)."""
    item_id = r.i16()
    if item_id == -1:
        return (-1, 0, 0, b"")
    count = r.u8()
    damage = r.i16()
    nbt_len = r.i16()
    nbt = r.read(nbt_len) if nbt_len > 0 else b""
    return (item_id, count, damage, nbt)


def write_slot125(w, slot):
    item_id, count, damage, nbt = slot
    w.i16(item_id)
    if item_id == -1:
        return
    w.u8(count)
    w.i16(damage)
    w.i16(len(nbt))
    w.raw(nbt)


def encode(packet_id, **fields):
    name, spec = PACKETS[packet_id]
    w = BufWriter()
    w.u8(packet_id)
    for fname, kind in spec:
        v = fields[fname]
        if kind == "b":
            w.i8(v)
        elif kind == "B":
            w.u8(v)
        elif kind == "h":
            w.i16(v)
        elif kind == "i":
            w.i32(v)
        elif kind == "q":
            w.i64(v)
        elif kind == "f":
            w.f32(v)
        elif kind == "d":
            w.f64(v)
        elif kind == "s":
            w.str125(v)
        elif kind == "?":
            w.bool(v)
        else:
            raise ValueError("bad field kind %r" % kind)
    return w.bytes()


def decode(packet_id, payload):
    """Decode a fixed-size payload (no id byte). Returns dict."""
    name, spec = PACKETS[packet_id]
    r = BufReader(payload)
    out = {}
    for fname, kind in spec:
        if kind == "b":
            out[fname] = r.i8()
        elif kind == "B":
            out[fname] = r.u8()
        elif kind == "h":
            out[fname] = r.i16()
        elif kind == "i":
            out[fname] = r.i32()
        elif kind == "q":
            out[fname] = r.i64()
        elif kind == "f":
            out[fname] = r.f32()
        elif kind == "d":
            out[fname] = r.f64()
        elif kind == "s":
            out[fname] = r.str125()
        elif kind == "?":
            out[fname] = r.bool()
        else:
            raise ValueError("bad field kind %r" % kind)
    if r.remaining():
        raise ValueError("%s: %d trailing bytes" % (name, r.remaining()))
    return out


def encode_mapchunk(x, z, groundup, primary, add, raw_deflated):
    w = BufWriter()
    w.u8(0x33)
    w.i32(x).i32(z).bool(groundup).i16(primary).i16(add)
    w.i32(len(raw_deflated))
    w.raw(raw_deflated)
    return w.bytes()


def encode_multiblock(x, z, records):
    """records: list of (lx, ly, lz, type, meta)."""
    w = BufWriter()
    w.u8(0x34)
    w.i32(x).i32(z)
    w.i16(len(records))
    w.i32(len(records) * 4)
    for lx, ly, lz, btype, meta in records:
        w.i16((lx << 12) | (lz << 8) | (ly & 0xFF))
        w.u8(btype).u8(meta)
    return w.bytes()


def encode_place(x, y, z, direction, slot):
    w = BufWriter()
    w.u8(0x0F)
    w.i32(x).u8(y).i32(z).i8(direction)
    write_slot125(w, slot)
    return w.bytes()


def decode_place(payload):
    r = BufReader(payload)
    out = {"x": r.i32(), "y": r.u8(), "z": r.i32(), "dir": r.i8()}
    out["slot"] = read_slot125(r)
    if r.remaining():
        raise ValueError("Place: trailing bytes")
    return out


def decode_login_client(payload):
    """C->S 0x01 Login (different layout than S->C)."""
    r = BufReader(payload)
    out = {"protocol": r.i32(), "username": r.str125(),
           "level": r.str125(), "servermode": r.i32(),
           "dim": r.i32(), "difficulty": r.u8(), "unused": r.u8(),
           "maxplayers": r.u8()}
    if r.remaining():
        raise ValueError("Login(C->S): trailing bytes")
    return out


class Connection125:
    """Blocking 1.2.5 endpoint (used for the game client side)."""

    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(120)

    def _readn(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("peer closed")
            buf += chunk
        return bytes(buf)

    def read_string(self):
        n = int.from_bytes(self._readn(2), "big", signed=True)
        if n < 0:
            raise ValueError("negative string")
        return self._readn(n * 2).decode("utf-16-be")

    def recv(self):
        """Returns (packet_id, payload_dict_or_raw). Manual packets return
        raw payload bytes under key _raw for their dedicated decoder."""
        pid = self._readn(1)[0]
        if pid == 0xFE:
            return pid, {}
        if pid == 0x02:  # handshake: single string
            return pid, {"data": self.read_string()}
        if pid == 0x03:
            return pid, {"text": self.read_string()}
        if pid == 0xFF:
            return pid, {"reason": self.read_string()}
        if pid == 0xC9:  # string + bool + short
            name = self.read_string()
            tail = self._readn(3)
            return pid, {"name": name, "online": tail[0] != 0,
                         "ping": int.from_bytes(tail[1:3], "big", signed=True)}
        if pid == 0x01:
            # Ambiguous fixed size (C->S and S->C share the id); the bridge
            # knows its direction, so return raw and let it pick a decoder.
            # C->S is 4+str+str+4+4+1+1+1, S->C is 4+str+1*5+1: read the
            # protocol int + username string first, then decide by length.
            raise ValueError("0x01 needs a directional decoder")
        if pid in (0x33,):
            raise ValueError("0x33 is server->client only here")
        if pid == 0x0F:
            # variable (slot): read fixed head, then slot
            head = self._readn(10)
            r = BufReader(head)
            out = {"x": r.i32(), "y": r.u8(), "z": r.i32(), "dir": r.i8()}
            item_id = int.from_bytes(self._readn(2), "big", signed=True)
            if item_id == -1:
                out["slot"] = (-1, 0, 0, b"")
                return pid, out
            rest = self._readn(3)
            count, damage = rest[0], int.from_bytes(rest[1:3], "big", signed=True)
            nbt_len = int.from_bytes(self._readn(2), "big", signed=True)
            nbt = self._readn(nbt_len) if nbt_len > 0 else b""
            out["slot"] = (item_id, count, damage, nbt)
            return pid, out
        if pid not in FIXED_SIZES or FIXED_SIZES[pid] is None:
            # String-bearing packets handled above; anything else unknown.
            raise ValueError("no decoder for 1.2.5 packet 0x%02X" % pid)
        size = FIXED_SIZES[pid]
        payload = self._readn(size) if size else b""
        return pid, decode(pid, payload)

    def recv_server_login(self):
        """Read a full S->C 0x01 Login including its id byte."""
        pid = self._readn(1)[0]
        if pid != 0x01:
            raise ValueError("expected 0x01 login, got 0x%02X" % pid)
        head = self._readn(4)
        eid = int.from_bytes(head, "big", signed=True)
        level = self.read_string()
        mode, dim, difficulty, unused, maxp = self._readn(5)
        return {"eid": eid, "level": level, "mode": mode, "dim": dim,
                "difficulty": difficulty, "unused": unused,
                "maxplayers": maxp}

    def recv_client_login(self):
        """Read a full C->S 0x01 Login including its id byte."""
        pid = self._readn(1)[0]
        if pid != 0x01:
            raise ValueError("expected 0x01 login, got 0x%02X" % pid)
        return self.recv_login_client()

    def recv_login_client(self):
        """Read a C->S 0x01 Login after its id byte was consumed."""
        proto = int.from_bytes(self._readn(4), "big", signed=True)
        username = self.read_string()
        level = self.read_string()
        servermode = int.from_bytes(self._readn(4), "big", signed=True)
        dim = int.from_bytes(self._readn(4), "big", signed=True)
        diff, unused, maxp = self._readn(3)
        return {"protocol": proto, "username": username, "level": level,
                "servermode": servermode, "dim": dim, "difficulty": diff,
                "unused": unused, "maxplayers": maxp}

    def send(self, data):
        self.sock.sendall(data)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
