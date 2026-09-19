#!/usr/bin/env python3
"""A hostile client, for fuzzing the relay server.

Hosting opens a UDP port on a pilot's own machine, so the server now parses
packets from anyone who finds it. This throws malformed, truncated,
inconsistent and outright random datagrams at it -- interleaved with real
sessions so the parser is exercised with live state around it -- and then
checks the server still works.

Run the server under AddressSanitizer and a single out-of-bounds read stops
the run with a stack trace.

    ./build-tests/xradio_server 49700 &
    python3 tools/harness/fuzz_client.py --port 49700
"""

import argparse
import os
import random
import socket
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "server"))

import protocol as P          # noqa: E402

TYPES = [P.PT_LOGIN, P.PT_LOGIN_ACK, P.PT_POSITION, P.PT_TRAFFIC, P.PT_TEXT,
         P.PT_VOICE, P.PT_PING, P.PT_PONG, P.PT_LOGOUT, 0, 10, 99, 255]

WILD_FLOATS = [float("nan"), float("inf"), float("-inf"), 0.0, -0.0,
               1e308, -1e308, 1e-308, 3.4e38, -3.4e38]


def rand_header(rng, ptype=None, sid=None, plen=None, magic=None, ver=None):
    return P.HEADER.pack(
        P.MAGIC if magic is None else magic,
        rng.choice(TYPES) if ptype is None else ptype,
        P.PROTO_VERSION if ver is None else ver,
        rng.randint(0, 65535) if plen is None else plen,
        rng.randint(0, 2**32 - 1) if sid is None else sid)


def mutate(rng, data):
    """Flip, truncate, extend or shuffle the bytes of a valid packet."""
    b = bytearray(data)
    if not b:
        return bytes(b)
    what = rng.randint(0, 4)
    if what == 0:                                   # flip some bits
        for _ in range(rng.randint(1, 8)):
            i = rng.randrange(len(b))
            b[i] ^= 1 << rng.randrange(8)
    elif what == 1:                                 # truncate
        b = b[:rng.randrange(len(b) + 1)]
    elif what == 2:                                 # extend
        b += os.urandom(rng.randint(1, 600))
    elif what == 3:                                 # lie about the length
        if len(b) >= P.HEADER.size:
            magic, t, v, _plen, sid = P.HEADER.unpack_from(b, 0)
            P.HEADER.pack_into(b, 0, magic, t, v, rng.randint(0, 65535), sid)
    else:                                           # swap a chunk around
        if len(b) > 8:
            i = rng.randrange(len(b) - 4)
            j = rng.randrange(len(b) - 4)
            b[i:i + 4], b[j:j + 4] = b[j:j + 4], b[i:i + 4]
    return bytes(b)


class Session:
    """A real client, so the server has live state while being attacked."""

    def __init__(self, port, callsign):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.4)
        self.dest = ("127.0.0.1", port)
        self.callsign = callsign
        self.sid = 0
        self.backlog = 0

    def login(self):
        self.sock.sendto(P.pack(P.PT_LOGIN, 0, P.login(self.callsign, "C172")),
            self.dest)
        try:
            data, _ = self.sock.recvfrom(2048)
        except socket.timeout:
            return 0
        parsed = P.unpack_header(data)
        if parsed and parsed[0] == P.PT_LOGIN_ACK:
            self.sid, _ = P.LOGIN_ACK.unpack_from(parsed[4], 0)
        return self.sid

    def position(self):
        self.sock.sendto(P.pack(P.PT_POSITION, self.sid, P.POSITION.pack(
            57.85, 27.02, 914.0, 90.0, 0.0, 0.0, 50.0, 0.0, 0.0,
            122800, 0, 0, 0, P.TX_COM1, P.RX_COM1, 0, 90.0, 0.0)), self.dest)

    def alive(self):
        """Ping and wait for the pong: proof the server is still serving.

        This socket has been collecting relayed traffic for the whole run and
        has a deep backlog, so empty it first -- otherwise the pong is sitting
        behind thousands of packets and a short read window never reaches it.
        """
        drained = 0
        self.sock.settimeout(0.05)
        while True:
            try:
                self.sock.recvfrom(2048)
            except socket.timeout:
                break
            drained += 1
            if drained > 200000:
                break
        self.backlog = drained

        self.sock.settimeout(0.4)
        for _ in range(10):
            self.sock.sendto(P.pack(P.PT_PING, self.sid), self.dest)
            end = time.time() + 0.4
            while time.time() < end:
                try:
                    data, _ = self.sock.recvfrom(2048)
                except socket.timeout:
                    break
                parsed = P.unpack_header(data)
                if parsed and parsed[0] == P.PT_PONG:
                    return True
        return False

    def raw(self, data):
        self.sock.sendto(data, self.dest)

    def close(self):
        self.sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=49700)
    ap.add_argument("--count", type=int, default=12000)
    ap.add_argument("--seed", type=int, default=20260919)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    good = Session(args.port, "GOODCL")
    if not good.login():
        print("the server did not answer a normal login -- is it running?")
        return 1
    good.position()

    attacker = Session(args.port, "EVILCL")
    attacker.login()

    sent = 0
    for i in range(args.count):
        pick = rng.randint(0, 9)
        if pick == 0:
            pkt = rand_header(rng) + os.urandom(rng.randint(0, 200))
        elif pick == 1:
            pkt = os.urandom(rng.randint(0, 1500))
        elif pick == 2:                                  # header, no payload
            pkt = rand_header(rng, plen=rng.randint(1, 65535))
        elif pick == 3:                                  # position, wild values
            vals = [rng.choice(WILD_FLOATS) for _ in range(2)] + \
                   [rng.choice(WILD_FLOATS) for _ in range(7)]
            try:
                body = P.POSITION.pack(vals[0], vals[1], *[float(v) for v in vals[2:9]],
                                       rng.randint(0, 2**32 - 1),
                                       rng.randint(0, 2**32 - 1),
                                       rng.randint(0, 255), rng.randint(0, 255),
                                       rng.randint(0, 255), rng.randint(0, 255),
                                       rng.randint(0, 2**32 - 1),
                                       rng.choice(WILD_FLOATS),
                                       rng.choice(WILD_FLOATS))
            except (OverflowError, struct.error):
                continue
            pkt = P.pack(P.PT_POSITION, attacker.sid, body)
        elif pick == 4:                                  # text, lying length
            body = P.TEXT_HDR.pack(122800, attacker.sid, P.pad("EVILCL", 16),
                                   rng.randint(0, 65535)) + os.urandom(rng.randint(0, 64))
            pkt = P.pack(P.PT_TEXT, attacker.sid, body)
        elif pick == 5:                                  # voice, lying length
            body = P.VOICE_HDR.pack(122800, attacker.sid, rng.randint(0, 65535),
                                    rng.randint(0, 65535)) + os.urandom(rng.randint(0, 64))
            pkt = P.pack(P.PT_VOICE, attacker.sid, body)
        elif pick == 6:                                  # login, junk strings
            body = os.urandom(rng.randint(0, 28))
            pkt = P.pack(P.PT_LOGIN, 0, body)
        elif pick == 7:                                  # a valid packet, mangled
            base = P.pack(P.PT_POSITION, attacker.sid, P.POSITION.pack(
                57.85, 27.02, 914.0, 90.0, 0.0, 0.0, 50.0, 0.0, 0.0,
                122800, 0, 0, 0, 1, 1, 0, 90.0, 0.0))
            pkt = mutate(rng, base)
        elif pick == 8:                                  # someone else's session
            pkt = P.pack(P.PT_TEXT, rng.randint(0, 2**32 - 1),
                         P.TEXT_HDR.pack(122800, 0, P.pad("SPOOF", 16), 4) + b"heyo")
        else:                                            # oversized payload
            pkt = P.pack(P.PT_TEXT, attacker.sid,
                         P.TEXT_HDR.pack(122800, 0, P.pad("BIG", 16), 60000) +
                         os.urandom(1200))

        attacker.raw(pkt)
        sent += 1

        # Keep a real session ticking over so the parser runs with live state
        # around it, not against an empty server.
        if i % 50 == 0:
            good.position()
        if i % 500 == 0:
            time.sleep(0.01)

    time.sleep(0.3)
    print(f"  sent {sent} malformed packets")

    still_here = good.alive()
    print(f"  a session that was live throughout had {good.backlog} packets queued")
    print("  the server is still answering" if still_here
          else "  THE SERVER STOPPED ANSWERING")

    fresh = Session(args.port, "AFTER1")
    joined = fresh.login() != 0
    print("  and still accepts new pilots" if joined
          else "  AND WILL NOT ACCEPT NEW PILOTS")

    good.close(); attacker.close(); fresh.close()
    return 0 if (still_here and joined) else 1


if __name__ == "__main__":
    sys.exit(main())
