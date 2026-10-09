"""The 1.2.5 <-> 1.8.9 session bridge.

One Bridge per game client: the client speaks 1.2.5 to us, we speak 1.8.9
to the real server. Two threads relay packets; every translated packet is
handled in _from_client / _from_server, everything else is skipped (1.8
framing allows skipping by length) or, on the 1.2.5 side, ends the session
(pre-Netty framing cannot resync past an unknown packet).

MVP scope: ping, login (offline-mode 1.8.9 servers), keepalive, chat,
movement, chunks, block changes, health, respawn, tab list, digging,
placing, held slot, use entity, animations. Dropped for phase 2:
entities/mobs/players (metadata rewrite), inventories/windows, online-mode
encryption, NBT round-trip fidelity (bytes pass through when well-formed).
"""

import socket
import threading
import zlib

from . import proto125 as p125
from . import proto189 as p189
from .proto189 import slot125_to_189, slot189_to_125
from .codec import BufReader, BufWriter, chat_to_text, decode_position_189, \
    encode_position_189, strip_section_codes
from .tables import ENTITY_ACTION, convert_chunk_sections, state_to_id_meta


def skip_metadata_189(r):
    """Consume a 1.8 Entity Metadata blob (index/type/value*, 0x7F ends)."""
    while True:
        index = r.u8()
        if index == 0x7F:
            return
        dtype = r.u8()
        if dtype == 0:
            r.u8()
        elif dtype == 1:
            r.i16()
        elif dtype == 2:
            r.i32()
        elif dtype == 3:
            r.f32()
        elif dtype == 4:
            r.str189()
        elif dtype == 5:
            p189.read_slot189(r)
        elif dtype == 6:
            r.i32()
            r.i32()
            r.i32()
        elif dtype == 7:
            r.f32()
            r.f32()
            r.f32()
        else:
            raise ValueError("bad metadata type %d" % dtype)


class Bridge:
    def __init__(self, client_sock, target_host, target_port, verbose=False):
        self.client = p125.Connection125(client_sock)
        self.target_host = target_host
        self.target_port = target_port
        self.verbose = verbose
        self.server = None
        self.stop = threading.Event()
        self.username = "?"
        self.eid = -1
        self.px = self.py = self.pz = 0.0
        self.pyaw = self.ppitch = 0.0
        self.gamemode = 0
        self.dimension = 0

    def log(self, *args):
        if self.verbose:
            print("[bridge:%s]" % self.username, *args, flush=True)

    # -- entry ----------------------------------------------------------

    def serve(self):
        """Read the first client byte: 0xFE = server-list ping, 0x02 = login."""
        try:
            first = self.client._readn(1)[0]
        except (ConnectionError, OSError):
            self.client.close()
            return
        try:
            if first == 0xFE:
                self.serve_ping()
            elif first == 0x02:
                data = self.client.read_string()
                self.serve_game(data)
            else:
                self.client.close()
        except Exception as e:  # noqa: BLE001 -- every failure is a kick
            self.log("error:", e)
            try:
                self.client.send(p125.encode(0xFF, reason="Bridge: %s" % e))
            except OSError:
                pass
            self.client.close()
            if self.server is not None:
                self.server.close()

    # -- ping -----------------------------------------------------------

    def serve_ping(self):
        srv = socket.create_connection((self.target_host, self.target_port), timeout=15)
        conn = p189.Connection189(srv)
        try:
            p189.send_handshake(conn, self.target_host, self.target_port, 1)
            js, _ping = p189.do_status(conn)
            motd, online, maxp = status_to_125(js)
            self.client.send(p125.encode(0xFF, reason="%s\u00a7%d\u00a7%d" % (motd, online, maxp)))
        finally:
            conn.close()
            self.client.close()

    # -- game -----------------------------------------------------------

    def serve_game(self, handshake_data):
        parts = handshake_data.split(";")
        self.username = parts[0] or "?"
        self.log("login from client as %r" % self.username)
        self.client.send(p125.encode(0x02, data="-"))
        login = self.client.recv_client_login()
        if login["protocol"] != 29:
            raise ValueError("client protocol %d is not 1.2.5" % login["protocol"])
        if login["username"] != self.username:
            self.username = login["username"]

        srv = socket.create_connection((self.target_host, self.target_port), timeout=20)
        self.server = p189.Connection189(srv)
        try:
            p189.send_handshake(self.server, self.target_host, self.target_port, 2)
            _uuid, name = p189.do_login(self.server, self.username)
            self.log("logged into 1.8.9 server as %r" % name)
        except p189.OnlineModeNeeded as e:
            raise ValueError(str(e))
        except ConnectionError as e:
            raise ValueError(str(e))

        threads = [threading.Thread(target=self._loop_c2s, daemon=True),
                   threading.Thread(target=self._loop_s2c, daemon=True)]
        for t in threads:
            t.start()
        self.stop.wait()
        self.client.close()
        self.server.close()

    def _loop_c2s(self):
        try:
            while not self.stop.is_set():
                pid, f = self.client.recv()
                self._from_client(pid, f)
        except Exception as e:  # noqa: BLE001
            self.log("c2s ended:", e)
        finally:
            self.stop.set()

    def _loop_s2c(self):
        try:
            while not self.stop.is_set():
                pid, r = self.server.recv()
                self._from_server(pid, r)
        except Exception as e:  # noqa: BLE001
            self.log("s2c ended:", e)
            try:
                self.client.send(p125.encode(0xFF, reason="Disconnected: %s" % e))
            except OSError:
                pass
        finally:
            self.stop.set()

    # -- client -> server -------------------------------------------------

    def _send189(self, packet_id, writer):
        self.server.send(packet_id, writer.bytes())

    def _from_client(self, pid, f):
        s = self.server
        if pid == 0x00:  # KeepAlive
            w = BufWriter().varint(f["id"] & 0xFFFFFFFF)
            self._send189(0x00, w)
        elif pid == 0x03:  # Chat
            w = BufWriter().str189(f["text"])
            self._send189(0x01, w)
        elif pid == 0x0A:  # Flying
            w = BufWriter().bool(f["ground"])
            self._send189(0x03, w)
        elif pid == 0x0B:  # Position
            self.px, self.py, self.pz = f["x"], f["y"], f["z"]
            w = BufWriter().f64(f["x"]).f64(f["y"]).f64(f["z"]).bool(f["ground"])
            self._send189(0x04, w)
        elif pid == 0x0C:  # Look
            self.pyaw, self.ppitch = f["yaw"], f["pitch"]
            w = BufWriter().f32(f["yaw"]).f32(f["pitch"]).bool(f["ground"])
            self._send189(0x05, w)
        elif pid == 0x0D:  # Position+Look (drop stance: 1.8 has none)
            self.px, self.py, self.pz = f["x"], f["y"], f["z"]
            self.pyaw, self.ppitch = f["yaw"], f["pitch"]
            w = BufWriter().f64(f["x"]).f64(f["y"]).f64(f["z"])
            w.f32(f["yaw"]).f32(f["pitch"]).bool(f["ground"])
            self._send189(0x06, w)
        elif pid == 0x0E:  # Digging
            w = BufWriter().varint(f["status"] & 0xFFFFFFFF)
            w.i64(encode_position_189(f["x"], f["y"], f["z"]))
            w.i8(f["face"])
            self._send189(0x07, w)
        elif pid == 0x0F:  # Block placement
            w = BufWriter()
            w.i64(encode_position_189(f["x"], f["y"], f["z"]))
            w.i8(f["dir"])
            p189.write_slot189(w, slot125_to_189(f["slot"]))
            w.u8(8).u8(8).u8(8)
            self._send189(0x08, w)
        elif pid == 0x10:  # Held slot
            w = BufWriter().i16(f["slot"])
            self._send189(0x09, w)
        elif pid == 0x12:  # Animation
            self._send189(0x0A, BufWriter())
        elif pid == 0x13:  # Entity action
            action = ENTITY_ACTION.get(f["action"])
            if action is None:
                return
            w = BufWriter().varint(self.eid & 0xFFFFFFFF)
            w.varint(action).varint(0)
            self._send189(0x0B, w)
        elif pid == 0x07:  # Use entity
            w = BufWriter().varint(f["target"] & 0xFFFFFFFF)
            w.varint(1 if f["left"] else 0)
            self._send189(0x02, w)
        elif pid == 0xCD:  # Respawn request
            w = BufWriter().varint(0)
            self._send189(0x16, w)
        elif pid == 0xFF:  # Quit
            raise ConnectionError("client quit")
        else:
            raise ValueError("unsupported 1.2.5 packet 0x%02X" % pid)
        _ = s

    # -- server -> client -------------------------------------------------

    def _from_server(self, pid, r):
        c = self.client
        if pid == 0x00:  # KeepAlive
            c.send(p125.encode(0x00, id=r.uvarint() & 0xFFFFFFFF))
        elif pid == 0x01:  # JoinGame -> 1.2.5 Login
            self.eid = r.i32()
            self.gamemode = r.u8() & 0x07
            self.dimension = r.i8()
            difficulty = r.u8()
            maxplayers = r.u8()
            level = r.str189()
            c.send(p125.encode(0x01, eid=self.eid, level=level or "default",
                               mode=self.gamemode, dim=self.dimension,
                               difficulty=difficulty, unused=0,
                               maxplayers=maxplayers))
        elif pid == 0x02:  # Chat
            text = chat_to_text(r.str189())
            _pos = r.u8()
            c.send(p125.encode(0x03, text=text[:119]))
        elif pid == 0x03:  # Time
            r.i64()  # world age
            c.send(p125.encode(0x04, time=r.i64()))
        elif pid == 0x05:  # Spawn position
            x, y, z = decode_position_189(r.i64())
            c.send(p125.encode(0x06, x=x, y=y, z=z))
        elif pid == 0x06:  # Health
            hp = max(0, min(20, int(r.f32())))
            food = max(0, min(20, r.uvarint()))
            c.send(p125.encode(0x08, hp=hp, food=food, sat=r.f32()))
        elif pid == 0x07:  # Respawn
            dim = r.i32()
            difficulty = r.u8()
            mode = r.u8() & 0x07
            level = r.str189()
            self.dimension = dim
            c.send(p125.encode(0x09, dim=dim & 0xFF, difficulty=difficulty,
                               mode=mode, height=256, level=level or "default"))
        elif pid == 0x08:  # Position+Look (flags: 1 bit per axis = relative)
            x, y, z = r.f64(), r.f64(), r.f64()
            yaw, pitch = r.f32(), r.f32()
            flags = r.u8()
            if flags & 0x01:
                x += self.px
            if flags & 0x02:
                y += self.py
            if flags & 0x04:
                z += self.pz
            if flags & 0x08:
                yaw += self.pyaw
            if flags & 0x10:
                pitch += self.ppitch
            self.px, self.py, self.pz = x, y, z
            self.pyaw, self.ppitch = yaw, pitch
            c.send(p125.encode(0x0D, x=x, y=y, stance=y + 1.62, z=z,
                               yaw=yaw, pitch=pitch, ground=False))
        elif pid == 0x0B:  # Animation
            c.send(p125.encode(0x12, eid=r.uvarint(), anim=r.u8()))
        elif pid == 0x21:  # Chunk data
            self._chunk(r)
        elif pid == 0x22:  # Multi block change
            cx, cz = r.i32(), r.i32()
            count = r.uvarint()
            records = []
            for _ in range(count):
                v = r.uvarint()
                lx = (v >> 28) & 0xF
                lz = (v >> 24) & 0xF
                ly = (v >> 12) & 0xFFF
                bid, meta = state_to_id_meta(v)
                records.append((lx, ly & 0xFF, lz, bid & 0xFF, meta))
            c.send(p125.encode_multiblock(cx, cz, records))
        elif pid == 0x23:  # Block change
            x, y, z = decode_position_189(r.i64())
            bid, meta = state_to_id_meta(r.uvarint())
            c.send(p125.encode(0x35, x=x, y=y & 0xFF, z=z,
                               type=bid & 0xFF, meta=meta))
        elif pid == 0x38:  # Player list
            self._playerlist(r)
        elif pid == 0x3F:  # Plugin message
            channel = r.str189()
            data = r.read(r.remaining())
            w = BufWriter().u8(0xFA).str125(channel)
            w.i16(len(data)).raw(data)
            c.send(w.bytes())
        elif pid == 0x40:  # Disconnect
            raise ConnectionError(chat_to_text(r.str189()))
        elif pid in (0x0C, 0x0E, 0x0F, 0x10, 0x14, 0x15, 0x16, 0x17, 0x18,
                     0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x24,
                     0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
                     0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x39,
                     0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x41, 0x42, 0x43, 0x44,
                     0x45, 0x46, 0x47, 0x48):
            # Phase 2 (entities, inventories, effects...) or cosmetic:
            # framing already consumed the whole packet, nothing to do.
            self.log("drop 1.8 packet 0x%02X (phase 2)" % pid)
        else:
            self.log("drop unknown 1.8 packet 0x%02X" % pid)

    def _chunk(self, r):
        cx, cz = r.i32(), r.i32()
        groundup = r.bool()
        bitmask = r.u16()
        size = r.uvarint()
        data = r.read(size)
        skylight = self.dimension == 0
        if bitmask == 0:
            self.client.send(p125.encode(0x32, x=cx, z=cz, load=False))
            return
        biome = b""
        if groundup:
            # 1.8 appends the 256 biome bytes after the section data.
            data, biome = data[:-256], data[-256:]
        primary, add, raw = convert_chunk_sections(bitmask, data, skylight)
        if groundup:
            raw += biome
        self.client.send(p125.encode(0x32, x=cx, z=cz, load=True))
        self.client.send(p125.encode_mapchunk(cx, cz, groundup, primary,
                                              add, zlib.compress(raw)))

    def _playerlist(self, r):
        action = r.uvarint()
        count = r.uvarint()
        for _ in range(count):
            if action == 0:  # ADD_PLAYER
                r.read(16)  # uuid
                name = r.str189()
                nprops = r.uvarint()
                for _p in range(nprops):
                    r.str189()
                    r.str189()
                    if r.bool():
                        r.str189()
                r.uvarint()  # gamemode
                r.uvarint()  # ping
                if r.bool():
                    r.str189()  # display name
                self.client.send(p125.encode(0xC9, name=name, online=True, ping=0))
            elif action == 1:  # UPDATE_GAMEMODE
                r.read(16)
                r.uvarint()
            elif action == 2:  # UPDATE_LATENCY
                r.read(16)
                r.uvarint()
            elif action == 3:  # UPDATE_DISPLAY_NAME
                r.read(16)
                if r.bool():
                    r.str189()
            elif action == 4:  # REMOVE_PLAYER
                r.read(16)
                self.client.send(p125.encode(0xC9, name="?", online=False, ping=0))
            else:
                raise ValueError("bad playerlist action %d" % action)


def status_to_125(status_json):
    """1.8 status JSON -> (motd_plain, online, max)."""
    import json as _json
    try:
        st = _json.loads(status_json)
    except ValueError:
        return "?", 0, 0
    desc = st.get("description", "")
    if isinstance(desc, dict):
        motd = chat_to_text(_json.dumps(desc))
    else:
        motd = str(desc)
    motd = strip_section_codes(motd).replace("\u00a7", "").replace("\n", " ")
    players = st.get("players", {}) or {}
    return motd[:60], int(players.get("online", 0)), int(players.get("max", 0))
