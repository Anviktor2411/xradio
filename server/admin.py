#!/usr/bin/env python3
"""Kick or mute a pilot, or set the pilot limit, on a running XRadio server.

The server has to have been started with an admin token (--admin-token, or
XRADIO_ADMIN_TOKEN); this sends PT_ADMIN with the same token and prints what
the server answered. It does not log in, so it never shows up in the flight.

    python3 admin.py --token s3cret mute ESNA12
    python3 admin.py --token s3cret unmute ESNA12
    python3 admin.py --token s3cret kick ESNA12 --minutes 30
    python3 admin.py --token s3cret limit 12          # 0: no limit
    python3 admin.py --host 203.0.113.7 --port 49100 --token s3cret kick ESNA12

A wrong token gets no answer at all -- the server says nothing to somebody
guessing -- so "no answer" here means a wrong token, a wrong address, or a
server without a token.
"""

import argparse
import os
import socket
import sys

import protocol as P

OPS = {"kick": P.ADMIN_KICK, "mute": P.ADMIN_MUTE, "unmute": P.ADMIN_UNMUTE, "limit": P.ADMIN_LIMIT}


def main():
    ap = argparse.ArgumentParser(description="XRadio server admin: kick, mute, unmute, limit")
    ap.add_argument("action", choices=sorted(OPS))
    ap.add_argument("what", help="the callsign (kick, mute, unmute), or the most pilots (limit, 0 = none)")
    ap.add_argument("--minutes", type=int, default=0,
                    help="kick: keep their address out this long (default 0: they may come straight back)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=int(os.environ.get("XRADIO_PORT", "49100")))
    ap.add_argument("--token", default=os.environ.get("XRADIO_ADMIN_TOKEN", ""),
                    help="the server's admin token (default: $XRADIO_ADMIN_TOKEN)")
    args = ap.parse_args()
    if not args.token:
        print("no token: pass --token or set XRADIO_ADMIN_TOKEN")
        return 2

    op = OPS[args.action]
    if op == P.ADMIN_LIMIT:
        try:
            value = max(0, int(args.what))
        except ValueError:
            print("limit wants a number of pilots, 0 for no limit")
            return 2
        payload = P.admin(op, args.token, value=value)
    else:
        payload = P.admin(op, args.token, target=args.what.upper(), minutes=max(0, min(args.minutes, 0xFFFF)))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)
    sock.sendto(P.pack(P.PT_ADMIN, 0, payload), (args.host, args.port))
    try:
        data, _ = sock.recvfrom(P.MAX_PACKET)
    except (socket.timeout, ConnectionResetError):
        print("no answer: wrong token, wrong address, or a server started without --admin-token")
        return 1
    parsed = P.unpack_header(data)
    if not parsed or parsed[0] != P.PT_ADMIN:
        print("an answer that is not one")
        return 1
    done = P.ADMIN.unpack_from(parsed[4], 0)[1] == 1
    if op == P.ADMIN_LIMIT:
        print(f"pilot limit: {value or 'none'}" if done else "the server did not take it")
    else:
        print(f"{args.action}: {args.what.upper()}" + (f" for {args.minutes} min" if op == P.ADMIN_KICK and args.minutes else "")
              if done else f"nobody called {args.what.upper()} is in the flight")
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
