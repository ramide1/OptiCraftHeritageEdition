# 1.2.5 → 1.8.9 translator proxy (MVP)

The game client speaks protocol 29 (1.2.5, pre-Netty) only. This proxy
listens for 1.2.5 connections locally, speaks protocol 47 (1.8.9, Netty
framing) to the real server, and translates both ways. Stdlib-only Python,
no dependencies.

## Run

```sh
py -m proxy --target <1.8.9-server> [--port 25565] [--listen 25564] [-v]
```

Then point the 1.2.5 client at `localhost:25564`. The server list ping
(`0xFE`) works too: the proxy queries the 1.8 status and answers in the
1.2.5 `MOTD§online§max` format.

## Selftest

```sh
py -m proxy.selftest   # fake 1.8.9 server <-> real bridge <-> fake 1.2.5 client
```

37 checks: framing round-trips, Position, JSON chat, NBT skip, chunk
section bytes, ping, login, keepalive, chat and movement both ways,
block change, health, tab list, disconnect.

## MVP scope (done)

Ping, login against **offline-mode** 1.8.9 servers, keepalive, chat,
movement/look, chunks (incl. `Add` nibbles for block ids > 255, biomes,
sky light), multi/single block change, health, respawn, tab list,
digging, block placing, held slot, use entity, animations, respawn
request, plugin-message forwarding, 1.8 compression (SetCompression).

## Phase 2 (not done)

- **Entities/mobs/other players**: dropped after consuming their bytes;
  needs the entity-metadata rewrite.
- **Inventories/windows**: clicks and window packets unhandled.
- **Online-mode servers**: the 1.8 side answers Encryption Request, the
  bridge aborts with a clear message. Needs RSA + AES/CFB8 (outside
  stdlib speed limits for play traffic — best done in the game or a
  compiled helper).
- **NBT fidelity**: well-formed NBT blobs pass through; exotic nested
  lists raise instead of corrupting.
- **Newer targets** (1.12, modern): needs the id/state remap tables
  (`proxy/tables.py` is where they land), registry/config-phase
  handling for 1.20.2+, and the 1.13 flattening wall.

## Files

- `codec.py` — VarInt, both string flavors, 1.8 Position, JSON-chat strip.
- `proto125.py` — pre-Netty framing + the 1.2.5 packet subset + slots.
- `proto189.py` — Netty framing + compression + slots/NBT skip + login.
- `tables.py` — id/meta helpers, chunk section conversion, action maps.
- `session.py` — the two-thread bridge and all packet translators.
- `__main__.py` — CLI listener. `selftest.py` — end-to-end checks.
