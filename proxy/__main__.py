"""OptiCraft 1.2.5 -> 1.8.9 translator proxy (MVP).

The 1.2.5 game client connects here speaking protocol 29; this proxy
speaks protocol 47 to the real 1.8.9 server and translates both ways.

Usage:
    py -m proxy --target some.1.8.server [--port 25565] [--listen 25564]

Then point the 1.2.5 client at localhost:<listen>. Server-list ping works
too (the proxy queries the 1.8 status and answers in 1.2.5 format).

MVP scope: offline-mode 1.8.9 servers; chat, movement, chunks, block
changes, health, respawn, tab list, digging/placing. Phase 2:
entities/mobs/players, inventories, online-mode encryption, NBT fidelity.
See proxy/README.md.
"""

import argparse
import socket
import threading

from .session import Bridge


def main():
    ap = argparse.ArgumentParser(description="1.2.5 -> 1.8.9 translator proxy (MVP)")
    ap.add_argument("--target", required=True, help="1.8.9 server host")
    ap.add_argument("--port", type=int, default=25565, help="1.8.9 server port")
    ap.add_argument("--listen", type=int, default=25564, help="local 1.2.5 port")
    ap.add_argument("--bind", default="127.0.0.1", help="local bind address")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.listen))
    srv.listen(16)
    print("1.2.5 -> 1.8.9 bridge: listening on %s:%d, target %s:%d"
          % (args.bind, args.listen, args.target, args.port), flush=True)
    print("point the 1.2.5 client at %s:%d" % (args.bind, args.listen), flush=True)
    while True:
        sock, addr = srv.accept()
        print("connection from %s" % (addr,), flush=True)
        bridge = Bridge(sock, args.target, args.port, verbose=args.verbose)
        threading.Thread(target=bridge.serve, daemon=True).start()


if __name__ == "__main__":
    main()
