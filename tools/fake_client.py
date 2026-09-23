#!/usr/bin/env python3
"""Fake XRadio client -- test the server without starting X-Plane.

Flies a straight line from a start position and prints the traffic it receives.
Start two of them on the same frequency to check that they see each other.

    python3 tools/fake_client.py --callsign ESNA12 --lat 57.85 --lon 27.02
    python3 tools/fake_client.py --callsign ESNB34 --lat 57.90 --lon 27.05 --heading 200
"""

import argparse
import math
import socket
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "server"))
import protocol as P  # noqa: E402


def run(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.2)
    dest = (args.server, args.port)

    sock.sendto(P.pack(P.PT_LOGIN, 0,
                       P.login(args.callsign, args.actype, password=args.password)), dest)

    sid = 0
    deadline = time.time() + 3.0
    while time.time() < deadline and sid == 0:
        try:
            data, _ = sock.recvfrom(P.MAX_PACKET)
        except socket.timeout:
            continue
        parsed = P.unpack_header(data)
        if parsed and parsed[0] == P.PT_LOGIN_ACK:
            sid, _t = P.LOGIN_ACK.unpack_from(parsed[4], 0)
    if sid == 0:
        print("no LOGIN_ACK -- is the server running, on this port, and does "
              "it want a password (--password)?")
        return
    print(f"logged in as {args.callsign}, sid={sid}")

    stop = threading.Event()
    voice_rx = [0]
    voice_tx = [0]

    def receiver():
        while not stop.is_set():
            try:
                data, _ = sock.recvfrom(P.MAX_PACKET)
            except socket.timeout:
                continue
            except OSError:
                return
            parsed = P.unpack_header(data)
            if not parsed:
                continue
            ptype, _v, _l, _s, payload = parsed
            if ptype == P.PT_TRAFFIC:
                count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
                off = P.TRAFFIC_HDR.size
                names = []
                for _ in range(count):
                    e = P.TRAFFIC_ENTRY.unpack_from(payload, off)
                    off += P.TRAFFIC_ENTRY.size
                    names.append(f"{P.cstr(e[1])}@{e[5] * 3.28084:.0f}ft"
                                 + ("[TX]" if e[14] else ""))
                if names:
                    print(f"  traffic: {', '.join(names)}")
            elif ptype == P.PT_TEXT:
                freq, _fs, frm, tlen = P.TEXT_HDR.unpack_from(payload, 0)
                text = payload[P.TEXT_HDR.size:P.TEXT_HDR.size + tlen]
                print(f"  [{freq / 1000:.3f}] {P.cstr(frm)}: "
                      f"{text.decode('utf-8', 'replace')}")
            elif ptype == P.PT_VOICE:
                freq, from_sid, seq, olen = P.VOICE_HDR.unpack_from(payload, 0)
                opus = payload[P.VOICE_HDR.size:P.VOICE_HDR.size + olen]
                voice_rx[0] += 1
                if voice_rx[0] == 1 or voice_rx[0] % 50 == 0:
                    print(f"  voice: {voice_rx[0]} frames from sid {from_sid} "
                          f"on {freq / 1000:.3f} ({olen} bytes)", flush=True)
                if args.parrot and opus:
                    # Send the same frame back as our own transmission, so a
                    # client can test its whole receive path against itself.
                    voice_tx[0] += 1
                    sock.sendto(P.pack(P.PT_VOICE, sid, P.VOICE_HDR.pack(
                        args.com1, sid, voice_tx[0] & 0xFFFF, len(opus)) + opus), dest)

    threading.Thread(target=receiver, daemon=True).start()

    lat, lon = args.lat, args.lon
    alt_m = args.alt_ft / 3.28084
    gs_ms = args.speed_kt * 0.514444
    tick = 0.2
    next_text = time.time() + 5.0
    try:
        while True:
            # dead-reckon along the heading
            d = gs_ms * tick / 6371000.0
            hdg = math.radians(args.heading)
            lat += math.degrees(d * math.cos(hdg))
            lon += math.degrees(d * math.sin(hdg) / max(0.01, math.cos(math.radians(lat))))

            sock.sendto(P.pack(P.PT_POSITION, sid, P.POSITION.pack(
                lat, lon,                                   # 2d
                alt_m, args.heading, 0.0, 0.0, gs_ms, 0.0, 0.0,   # 7f
                args.com1, 0,                               # 2I: com1, com2
                0,                                          # lights
                0,                                          # onGround
                P.TX_COM1 if args.talk else P.TX_NONE,      # txRadio
                P.RX_COM1,                                  # rxMask
                int(time.monotonic() * 1000) & 0xFFFFFFFF,  # timeMs
                args.heading,                               # trackTrue
                0.0,                                        # vsMs
                args.squawk,                                # squawk
                args.xpdr,                                  # xpdrMode
                0)), dest)                                  # xpdrIdent

            # Text names its own frequency and does not need the PTT held.
            if args.talk and time.time() >= next_text:
                msg = f"{args.callsign} position report".encode()
                sock.sendto(P.pack(P.PT_TEXT, sid, P.TEXT_HDR.pack(
                    args.com1, sid, P.pad(args.callsign, 16), len(msg)) + msg), dest)
                next_text = time.time() + 5.0

            time.sleep(tick)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        sock.sendto(P.pack(P.PT_LOGOUT, sid), dest)
        sock.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=49100)
    ap.add_argument("--callsign", default="TEST01")
    ap.add_argument("--password", default="",
                    help="the flight password, if the server wants one")
    ap.add_argument("--actype", default="C172")
    ap.add_argument("--lat", type=float, default=57.85)
    ap.add_argument("--lon", type=float, default=27.02)
    ap.add_argument("--alt-ft", type=float, default=3000.0)
    ap.add_argument("--heading", type=float, default=90.0)
    ap.add_argument("--speed-kt", type=float, default=110.0)
    ap.add_argument("--com1", type=int, default=122800, help="kHz, e.g. 122800")
    ap.add_argument("--squawk", type=int, default=1200, help="four octal digits")
    ap.add_argument("--xpdr", type=int, default=P.XPDR_ALT,
                    help="transponder mode: 0 off, 1 stby, 2 on, 3 alt")
    ap.add_argument("--talk", action="store_true", help="send a text message every 5 s")
    ap.add_argument("--parrot", action="store_true",
                    help="re-transmit any voice frames received, for loopback tests")
    run(ap.parse_args())
