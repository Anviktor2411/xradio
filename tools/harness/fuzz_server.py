#!/usr/bin/env python3
"""A deliberately hostile XRadio server.

Accepts a login, then answers every packet with malformed, truncated,
inconsistent or random data. The plugin parses this straight off the wire, so
running the harness against this (ideally built with -fsanitize=address)
proves the parser cannot be made to read out of bounds.

    python3 tools/harness/fuzz_server.py --port 49300
"""

import argparse
import random
import socket
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "server"))
import protocol as P  # noqa: E402


def raw(ptype, sid, payload, declared_len=None):
    """Build a packet, optionally lying about the payload length."""
    n = len(payload) if declared_len is None else declared_len
    return P.HEADER.pack(P.MAGIC, ptype, P.PROTO_VERSION, n & 0xFFFF, sid) + payload


def traffic_entry(callsign=b"FUZZ01", **kw):
    return P.TRAFFIC_ENTRY.pack(
        kw.get("sid", 7), P.pad(callsign.decode(), 16), P.pad("C172", 8),
        kw.get("lat", 57.85), kw.get("lon", 27.02),
        kw.get("alt", 900.0), 90.0, 0.0, 0.0, 50.0, 0.0, 0.0,
        0, 0, kw.get("tx", 0), kw.get("xpdr", P.XPDR_ALT),
        kw.get("t", 1000), kw.get("track", 90.0), kw.get("vs", 0.0),
        P.pad(kw.get("livery", ""), 16),
        kw.get("squawk", 1200), kw.get("ident", 0), 0)


def nasty_packets(sid, rnd):
    """Every way we can think of to lie to the client."""
    e = traffic_entry()
    yield "traffic: count says 65535, one entry present", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(0xFFFF, 0) + e)
    yield "traffic: count says 100, no entries", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(100, 0))
    yield "traffic: entry cut in half", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) + e[:len(e) // 2])
    yield "traffic: header only, no count field", \
        raw(P.PT_TRAFFIC, sid, b"\x01")
    yield "traffic: declared length far beyond the datagram", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) + e, declared_len=60000)
    yield "traffic: NaN and infinite coordinates", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) +
            traffic_entry(lat=float("nan"), lon=float("inf"), alt=float("-inf")))
    yield "traffic: callsign with no null terminator", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) +
            P.TRAFFIC_ENTRY.pack(9, b"A" * 16, b"B" * 8, 57.0, 27.0,
                                 900.0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0,
                                 b"L" * 16, 0, 0, 0))
    yield "traffic: NaN track and absurd vertical speed", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) +
            traffic_entry(track=float("nan"), vs=1e9))
    yield "traffic: timestamp going backwards", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) + traffic_entry(t=5))
    yield "traffic: a squawk no transponder could produce", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) +
            traffic_entry(squawk=65535, xpdr=255, ident=255))
    yield "traffic: timestamp at the 32-bit wrap", \
        raw(P.PT_TRAFFIC, sid, P.TRAFFIC_HDR.pack(1, 0) + traffic_entry(t=0xFFFFFFFF))

    yield "text: textLen says 65535, few bytes present", \
        raw(P.PT_TEXT, sid, P.TEXT_HDR.pack(122800, 1, P.pad("X", 16), 0xFFFF, P.pad("", 16)) + b"hi")
    yield "text: textLen 0", \
        raw(P.PT_TEXT, sid, P.TEXT_HDR.pack(122800, 1, P.pad("X", 16), 0, P.pad("", 16)))
    yield "text: header truncated mid-struct", \
        raw(P.PT_TEXT, sid, P.TEXT_HDR.pack(122800, 1, P.pad("X", 16), 5, P.pad("", 16))[:10])
    yield "text: invalid UTF-8 body", \
        raw(P.PT_TEXT, sid, P.TEXT_HDR.pack(122800, 1, P.pad("X", 16), 6, P.pad("", 16)) +
            b"\xff\xfe\xfd\xfc\xfb\xfa")
    yield "text: 1200 bytes of body", \
        raw(P.PT_TEXT, sid, P.TEXT_HDR.pack(122800, 1, P.pad("X", 16), 1200, P.pad("", 16)) + b"Z" * 1200)
    yield "text: embedded null bytes and format specifiers", \
        raw(P.PT_TEXT, sid, P.TEXT_HDR.pack(122800, 1, P.pad("X", 16), 12, P.pad("", 16)) +
            b"%s%n%x\x00\x00abc")

    yield "login_ack: payload too short", raw(P.PT_LOGIN_ACK, sid, b"\x01\x02")
    yield "voice: junk payload", raw(P.PT_VOICE, sid, b"\x00" * 40)
    yield "voice: opusLen says 65535, few bytes", \
        raw(P.PT_VOICE, sid, P.VOICE_HDR.pack(122800, 3, 1, 0xFFFF) + b"\xfc\xff\xfe")
    yield "voice: opusLen 0", raw(P.PT_VOICE, sid, P.VOICE_HDR.pack(122800, 3, 2, 0))
    yield "voice: header cut short", raw(P.PT_VOICE, sid, P.VOICE_HDR.pack(122800, 3, 3, 10)[:6])
    yield "voice: random bytes as an Opus frame", \
        raw(P.PT_VOICE, sid, P.VOICE_HDR.pack(122800, 3, 4, 120) +
            bytes(rnd.randrange(256) for _ in range(120)))
    yield "voice: 1300-byte frame", \
        raw(P.PT_VOICE, sid, P.VOICE_HDR.pack(122800, 3, 5, 1300) + b"\x80" * 1300)
    yield "voice: sequence going backwards", \
        raw(P.PT_VOICE, sid, P.VOICE_HDR.pack(122800, 3, 0, 3) + b"\xfc\xff\xfe")
    # A stream per session id costs the client a decoder: try to make it
    # allocate a thousand of them.
    for k in range(50):
        yield f"voice: session id {1000 + k}", \
            raw(P.PT_VOICE, sid, P.VOICE_HDR.pack(122800, 1000 + k + rnd.randrange(100000), 1, 3)
                + b"\xfc\xff\xfe")
    yield "unknown packet type 200", raw(200, sid, b"junk")
    yield "header only, zero payload", raw(P.PT_TRAFFIC, sid, b"")
    yield "single byte", b"\x58"
    yield "correct magic, random tail", \
        P.HEADER.pack(P.MAGIC, rnd.randrange(256), P.PROTO_VERSION,
                      rnd.randrange(65536), sid) + bytes(rnd.randrange(256)
                                                         for _ in range(rnd.randrange(64)))
    yield "pure random bytes", bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 200)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=49300)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rnd = random.Random(args.seed)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    sock.settimeout(1.0)
    print(f"hostile server on {args.port}", flush=True)

    peers = {}
    sent = 0
    while True:
        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break

        parsed = P.unpack_header(data)
        if parsed is None:
            continue
        ptype = parsed[0]

        if ptype == P.PT_LOGIN:
            peers[addr] = len(peers) + 1
            sid = peers[addr]
            sock.sendto(P.pack(P.PT_LOGIN_ACK, sid, P.LOGIN_ACK.pack(sid, 0)), addr)
            print(f"  accepted {addr} as sid {sid}", flush=True)
            continue

        sid = peers.get(addr, 1)
        # Answer each position report with the next piece of garbage.
        for name, pkt in nasty_packets(sid, rnd):
            sock.sendto(pkt, addr)
            sent += 1
        if sent % 100 < 21:
            print(f"  sent {sent} malformed packets", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
