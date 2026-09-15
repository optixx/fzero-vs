#!/usr/bin/env python3
"""Thin sync client for the local ares debug bridge; see docs/ares-debug-protocol.md.

Usage:
  scripts/ares_bridge.py status
  scripts/ares_bridge.py screenshot out.png
  scripts/ares_bridge.py read --domain rom --address 0x67800 --size 1024 [--out dump.hex]
  scripts/ares_bridge.py pause | resume
"""

import argparse
import json
import pathlib
import shutil
import socket
import sys

DEFAULT_SOCKET = "/tmp/fzero-vs-ares-debug.sock"


def request(sock, command, **params):
    sock.sendall((json.dumps({"id": 1, "command": command, **params}) + "\n").encode())
    line = b""
    while not line.endswith(b"\n"):
        chunk = sock.recv(1 << 16)
        if not chunk:
            raise SystemExit("bridge closed the connection")
        line += chunk
    response = json.loads(line)
    if not response.get("ok"):
        raise SystemExit(f"{command} failed: {response.get('error')}")
    return {k: v for k, v in response.items() if k not in ("id", "ok")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    shot = sub.add_parser("screenshot")
    shot.add_argument("output", type=pathlib.Path)
    read = sub.add_parser("read")
    read.add_argument("--domain", required=True,
                      choices=("wram", "vram", "oam", "cgram", "rom", "bus"))
    read.add_argument("--address", required=True, type=lambda v: int(v, 0))
    read.add_argument("--size", required=True, type=lambda v: int(v, 0))
    read.add_argument("--out", type=pathlib.Path, help="write raw bytes here instead of stdout")
    sub.add_parser("pause")
    sub.add_parser("resume")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(15)
    sock.connect(args.socket)
    try:
        if args.command == "status":
            print(json.dumps(request(sock, "emulator.status"), indent=2))
        elif args.command == "screenshot":
            meta = request(sock, "screen.capture")
            print(json.dumps(meta))
            shutil.copyfile(meta["path"], args.output)
            print(f"saved {args.output}")
        elif args.command == "read":
            meta = request(sock, "memory.read", domain=args.domain,
                           address=args.address, size=args.size)
            data = bytes.fromhex(meta["data"])
            if args.out:
                args.out.write_bytes(data)
                print(f"wrote {len(data)} bytes to {args.out}")
            else:
                for offset in range(0, len(data), 16):
                    print(f"{args.address + offset:06x}  {data[offset:offset + 16].hex(' ')}")
        elif args.command == "pause":
            print(json.dumps(request(sock, "emulator.pause")))
        elif args.command == "resume":
            print(json.dumps(request(sock, "emulator.resume")))
    finally:
        sock.close()


if __name__ == "__main__":
    main()
