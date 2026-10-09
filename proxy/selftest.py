"""End-to-end selftest: fake 1.8.9 server <-> real Bridge <-> fake 1.2.5
client. Covers ping, login, keepalive, chat both ways, movement, chunk
conversion bytes, block change, health, tab list, disconnect, plus unit
checks for VarInt/strings/Position/JSON-chat/NBT-skip.

Run:  py -m proxy.selftest   (exit 0 = ALL PASS)
"""

import socket
import struct
import threading
import zlib

from . import proto125 as p125
from . import proto189 as p189
from .codec import BufReader, BufWriter, chat_to_text, decode_position_189, \
    encode_position_189
from .session import Bridge, status_to_125
from .tables import convert_chunk_sections

PASS = []


def check(cond, name):
    print(("ok: " if cond else "FAIL: ") + name, flush=True)
    if cond:
        PASS.append(name)
    else:
        raise AssertionError(name)


# -- unit checks ----------------------------------------------------------

v = BufWriter().varint(300).bytes()
check(BufReader(v).uvarint() == 300, "varint roundtrip")
v = BufWriter().varint(2147483647).bytes()
check(BufReader(v).uvarint() == 2147483647, "varint max")
w = BufWriter().str125("TestPlayer")
check(BufReader(w.bytes()).str125() == "TestPlayer", "str125 roundtrip")
w = BufWriter().str189("hola se\u00f1or")
check(BufReader(w.bytes()).str189() == "hola se\u00f1or", "str189 roundtrip")
for x, y, z in ((0, 64, 0), (-5, -3, 129), (30000000, 255, -30000000)):
    check(decode_position_189(encode_position_189(x, y, z)) == (x, y, z),
          "position roundtrip %d,%d,%d" % (x, y, z))
check(chat_to_text('{"text":"hi","extra":[{"text":" there"}]}') == "hi there",
      "json chat extra")
check(chat_to_text('{"text":"\u00a7aGreen"}') == "\u00a7aGreen",
      "json chat keeps section codes")
check(chat_to_text("plain") == "plain", "non-json chat passthrough")
check(status_to_125('{"description":{"text":"Test MOTD"},"players":{"online":5,"max":20}}')
      == ("Test MOTD", 5, 20), "status json -> 1.2.5")

# NBT skip: compound { string "name" = "AB" }
nbt = bytes([10, 0, 0, 8, 0, 4]) + b"name" + bytes([0, 2]) + b"AB" + bytes([0])
r = BufReader(bytes([0, 5, 0, 0, 0]) + nbt)  # slot id=5,count=0,damage=0 then NBT
r.i16()
r.u8()
r.i16()
got = p189.read_nbt189(r)
check(got == nbt and r.remaining() == 0, "nbt blob skip")

# chunk section conversion: 1 section of stone, full sky
sec = b"".join(struct.pack(">H", (1 << 4)) for _ in range(4096))
sec += b"\x00" * 2048 + b"\xff" * 2048
primary, add, raw = convert_chunk_sections(0x0001, sec, True)
check(primary == 1 and add == 0, "chunk masks")
check(raw[:4096] == b"\x01" * 4096, "chunk block ids")
check(raw[4096:4096 + 2048] == b"\x00" * 2048, "chunk meta nibbles")
check(raw[4096 + 2048:4096 + 4096] == b"\x00" * 2048, "chunk block light")
check(raw[4096 + 4096:4096 + 6144] == b"\xff" * 2048, "chunk sky light")


# -- fake 1.8.9 server -----------------------------------------------------

def send189(sock, pid, payload):
    body = BufWriter().varint(pid).raw(payload).bytes()
    sock.sendall(BufWriter().varint(len(body)).raw(body).bytes())


def run_fake_server(listener, script):
    conn, _ = listener.accept()
    c = p189.Connection189(conn)
    try:
        script(c)
    finally:
        c.close()


def status_script(c):
    pid, r = c.recv()
    assert pid == 0x00, pid
    _proto, _host, _port, state = r.uvarint(), r.str189(), r.u16(), r.uvarint()
    assert state == 1, state
    pid, _r = c.recv()
    assert pid == 0x00, pid
    js = '{"description":{"text":"Test MOTD"},"players":{"online":5,"max":20},"version":{"name":"1.8.9","protocol":47}}'
    send189(c.sock, 0x00, BufWriter().str189(js).bytes())
    pid, r = c.recv()
    assert pid == 0x01, pid
    send189(c.sock, 0x01, BufWriter().i64(r.i64()).bytes())


SERVER_GOT = {}


def game_script(c):
    pid, r = c.recv()  # handshake
    assert pid == 0x00, pid
    assert r.uvarint() == 47, "protocol"
    r.str189()
    r.u16()
    assert r.uvarint() == 2, "login state"
    pid, r = c.recv()  # login start
    assert pid == 0x00, pid
    assert r.str189() == "TestPlayer", "username"
    w = BufWriter().str189("00000000-0000-0000-0000-000000000000").str189("TestPlayer")
    send189(c.sock, 0x02, w.bytes())  # login success

    # JoinGame -> client must emit 1.2.5 Login
    w = BufWriter()
    w.i32(1234).u8(0).i8(0).u8(1).u8(10).str189("default")
    send189(c.sock, 0x01, w.bytes())
    # SpawnPosition
    send189(c.sock, 0x05, BufWriter().i64(encode_position_189(100, 64, -30)).bytes())
    # TimeUpdate
    send189(c.sock, 0x03, BufWriter().i64(1000).i64(6000).bytes())
    # Chat
    send189(c.sock, 0x02, BufWriter().str189('{"text":"hi 1.2.5"}').u8(0).bytes())
    # ChunkData: section 0 all stone, sky full, biome zeros, groundUp
    sec = b"".join(struct.pack(">H", 1 << 4) for _ in range(4096))
    sec += b"\x00" * 2048 + b"\xff" * 2048
    chunk = sec + b"\x00" * 256
    w = BufWriter().i32(0).i32(0).bool(True).u16(1).varint(len(chunk)).raw(chunk)
    send189(c.sock, 0x21, w.bytes())
    # PlayerList ADD_PLAYER
    w = BufWriter().varint(0).varint(1)
    w.raw(b"\x00" * 16).str189("TestPlayer").varint(0).varint(0).varint(10).bool(False)
    send189(c.sock, 0x38, w.bytes())
    # KeepAlive
    send189(c.sock, 0x00, BufWriter().varint(77).bytes())

    # expect client chat + movement back
    pid, r = c.recv()
    assert pid == 0x01, pid
    SERVER_GOT["chat"] = r.str189()
    pid, r = c.recv()
    assert pid == 0x06, pid
    SERVER_GOT["pos"] = (r.f64(), r.f64(), r.f64(), r.f32(), r.f32(), r.bool())

    # block change + health + disconnect
    send189(c.sock, 0x23, BufWriter().i64(encode_position_189(101, 65, -30))
            .varint((3 << 4) | 2).bytes())
    send189(c.sock, 0x06, BufWriter().f32(20.0).varint(20).f32(5.0).bytes())
    send189(c.sock, 0x40, BufWriter().str189('{"text":"bye"}').bytes())


def read_u8(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return bytes(buf)


def client_login_blob():
    w = BufWriter()
    w.i32(29).str125("TestPlayer").str125("default")
    w.i32(0).i32(0).u8(0).u8(0).u8(1)
    return b"\x01" + w.bytes()


# -- run --------------------------------------------------------------------

listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(4)
port = listener.getsockname()[1]

# 1) ping
t = threading.Thread(target=run_fake_server, args=(listener, status_script))
t.start()
csock, bsock = socket.socketpair()
bt = threading.Thread(target=Bridge(bsock, "127.0.0.1", port).serve)
bt.start()
csock.sendall(b"\xfe")
pid = read_u8(csock, 1)[0]
assert pid == 0xFF, pid
n = int.from_bytes(read_u8(csock, 2), "big", signed=True)
reason = read_u8(csock, n * 2).decode("utf-16-be")
check(reason == "Test MOTD\u00a75\u00a720", "ping reply format: %r" % reason)
csock.close()
t.join(timeout=10)
bt.join(timeout=10)

# 2) game
t = threading.Thread(target=run_fake_server, args=(listener, game_script))
t.start()
csock, bsock = socket.socketpair()
bt = threading.Thread(target=Bridge(bsock, "127.0.0.1", port).serve)
bt.start()
fake = p125.Connection125(csock)
csock.sendall(p125.encode(0x02, data="TestPlayer;localhost:1"))
pid, f = fake.recv()
check(pid == 0x02 and f["data"] == "-", "handshake offline ack")
csock.sendall(client_login_blob())

f = fake.recv_server_login()
check(f["eid"] == 1234 and f["dim"] == 0, "joingame -> login")
pid, f = fake.recv()
check(pid == 0x06 and (f["x"], f["y"], f["z"]) == (100, 64, -30), "spawn pos")
pid, f = fake.recv()
check(pid == 0x04 and f["time"] == 6000, "time update")
pid, f = fake.recv()
check(pid == 0x03 and f["text"] == "hi 1.2.5", "chat json -> text")
pid, f = fake.recv()
check(pid == 0x32 and f["load"] is True, "prechunk load")
raw33 = read_u8(csock, 1)
assert raw33 == b"\x33", raw33
cx = int.from_bytes(read_u8(csock, 4), "big", signed=True)
cz = int.from_bytes(read_u8(csock, 4), "big", signed=True)
groundup = read_u8(csock, 1) != b"\x00"
primary = int.from_bytes(read_u8(csock, 2), "big", signed=True)
add = int.from_bytes(read_u8(csock, 2), "big", signed=True)
size = int.from_bytes(read_u8(csock, 4), "big", signed=True)
payload = zlib.decompress(read_u8(csock, size))
check((cx, cz, groundup, primary, add) == (0, 0, True, 1, 0), "mapchunk header")
check(payload[:4096] == b"\x01" * 4096, "mapchunk stone ids")
check(payload[4096 + 2048:4096 + 4096] == b"\x00" * 2048, "mapchunk light")
check(payload[4096 + 4096:4096 + 6144] == b"\xff" * 2048, "mapchunk skylight")
check(payload[-256:] == b"\x00" * 256, "mapchunk biome")
pid, f = fake.recv()
check(pid == 0xC9 and f["name"] == "TestPlayer" and f["online"] is True, "tab add")
pid, f = fake.recv()
check(pid == 0x00 and f["id"] == 77, "keepalive relay")

csock.sendall(p125.encode(0x03, text="hello server"))
csock.sendall(p125.encode(0x0D, x=100.5, y=65.0, stance=66.62, z=-29.5,
                          yaw=90.0, pitch=0.0, ground=True))
t.join(timeout=15)
check(SERVER_GOT.get("chat") == "hello server", "chat client -> server")
px, py, pz, yaw, pitch, ground = SERVER_GOT["pos"]
check(abs(px - 100.5) < 1e-6 and abs(py - 65.0) < 1e-6 and abs(pz + 29.5) < 1e-6
      and ground is True, "movement relay")
check(abs(yaw - 90.0) < 1e-3, "look relay")

pid, f = fake.recv()
check(pid == 0x35 and (f["x"], f["y"], f["z"], f["type"], f["meta"])
      == (101, 65, -30, 3, 2), "block change")
pid, f = fake.recv()
check(pid == 0x08 and (f["hp"], f["food"]) == (20, 20), "health")
pid, f = fake.recv()
check(pid == 0xFF and f["reason"] == "Disconnected: bye", "disconnect text")
csock.close()
bt.join(timeout=10)
listener.close()

print("ALL PASS (%d checks)" % len(PASS), flush=True)
