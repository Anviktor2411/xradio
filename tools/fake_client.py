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

    flags = (P.LF_ATC if args.atc else 0) | (P.LF_INJECTOR if args.inject else 0)
    sock.sendto(P.pack(P.PT_LOGIN, 0,
                       P.login(args.callsign, "ATC" if args.atc else args.actype,
                               password=args.password, flags=flags)), dest)

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

    # v6: file a flight plan straight after logging in ("EETN EFHK DCT ...")
    if args.file_plan:
        parts = args.file_plan.split()
        dep, dst = (parts + ["", ""])[:2]
        route = " ".join(parts[2:])
        sock.sendto(P.pack(P.PT_FLIGHTPLAN, sid, P.flightplan(
            args.callsign, args.actype, dep, dst, route, rules=args.rules,
            cruise_ft=args.cruise, remarks=args.remarks)), dest)
        print(f"filed flight plan {dep} -> {dst} {route}".rstrip())

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
                freq, _fs, frm, tlen, to = P.TEXT_HDR.unpack_from(payload, 0)
                text = payload[P.TEXT_HDR.size:P.TEXT_HDR.size + tlen]
                where = "direct" if P.cstr(to) else f"{freq / 1000:.3f}"
                print(f"  [{where}] {P.cstr(frm)}: "
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
            # --- v6 ---
            elif ptype == P.PT_ATC_LIST:
                count, _ = P.ATC_LIST_HDR.unpack_from(payload, 0)
                off, names = P.ATC_LIST_HDR.size, []
                for _ in range(count):
                    e = P.ATC_ENTRY.unpack_from(payload, off)
                    off += P.ATC_ENTRY.size
                    kind = {P.ATC_DEL: "DEL", P.ATC_GND: "GND", P.ATC_TWR: "TWR", P.ATC_APP: "APP",
                            P.ATC_CTR: "CTR", P.ATC_ATIS: "ATIS"}.get(e[4], "ATC")
                    names.append(f"{P.cstr(e[1])} {e[3] / 1000:.3f} ({kind})")
                line = "  atc: " + (", ".join(names) if names else "nobody")
                if line != atc_last[0]:
                    atc_last[0] = line
                    print(line, flush=True)
            elif ptype == P.PT_ATIS:
                freq, icao, letter, _r, tlen = P.ATIS_HDR.unpack_from(payload, 0)
                text = payload[P.ATIS_HDR.size:P.ATIS_HDR.size + tlen].decode("utf-8", "replace")
                print(f"  ATIS {P.cstr(icao)} information {letter.decode()} on {freq / 1000:.3f}: {text}", flush=True)
            elif ptype == P.PT_FLIGHTPLAN:
                d = P.unpack_flightplan(payload)
                if d:
                    st = {P.FP_FILED: "filed", P.FP_ACK: "acknowledged", P.FP_CLEARED: "cleared",
                          P.FP_CANCELLED: "cancelled"}.get(d["status"], str(d["status"]))
                    print(f"  flight plan {d['callsign']} {d['dep']}->{d['dest']} {d['route']}: {st}"
                          f"{', squawk %04d' % d['squawk'] if d['squawk'] else ''}"
                          f"{' -- ' + d['remarks'] if d['remarks'] else ''}", flush=True)
                    # a station standing in for a controller: every filed plan is
                    # acknowledged with a squawk, so a client can be tested end to end
                    if args.atc and args.ack_plans and d["status"] == P.FP_FILED and d["session_id"]:
                        sock.sendto(P.pack(P.PT_FLIGHTPLAN, sid, P.flightplan(
                            d["callsign"], d["ac_icao"], d["dep"], d["dest"], d["route"], rules=d["rules"],
                            cruise_ft=d["cruise_ft"], altn=d["altn"], status=P.FP_ACK, squawk=args.ack_plans,
                            session_id=d["session_id"], remarks=f"cleared to {d['dest']} as filed")), dest)
                        print(f"  acknowledged {d['callsign']} with squawk {args.ack_plans:04d}", flush=True)
            elif ptype == P.PT_WEATHER:
                if len(payload) == P.WEATHER.size:
                    w = P.WEATHER.unpack_from(payload, 0)
                    n_cloud = 4 * P.CLOUD_LAYERS
                    wind_spd = w[10 + n_cloud + P.AIR_LAYERS]
                    wind_dir = w[10 + n_cloud + 2 * P.AIR_LAYERS]
                    print(f"  weather: vis {w[3]:.1f} sm, QNH {w[4] / 100:.0f} hPa, {w[5]:.0f} C, "
                          f"wind {wind_dir:.0f}/{wind_spd * 1.94384:.0f} kt", flush=True)

    atc_last = [""]
    threading.Thread(target=receiver, daemon=True).start()

    if args.direct:
        to, _, body = args.direct.partition(" ")
        body = (body or "hello").encode()
        sock.sendto(P.pack(P.PT_TEXT, sid, P.TEXT_HDR.pack(
            0, sid, P.pad(args.callsign, 16), len(body), P.pad(to.upper(), 16)) + body), dest)

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
                args.com1, args.com2,                       # 2I: com1, com2
                0,                                          # lights
                1 if args.atc else 0,                       # onGround
                P.TX_COM1 if args.talk else P.TX_NONE,      # txRadio
                P.RX_COM1 | (P.RX_COM2 if args.com2 else 0),   # rxMask
                int(time.monotonic() * 1000) & 0xFFFFFFFF,  # timeMs
                args.heading,                               # trackTrue
                0.0,                                        # vsMs
                args.squawk,                                # squawk
                args.xpdr,                                  # xpdrMode
                0)), dest)                                  # xpdrIdent

            # v6: an injector drives a row of aircraft east of itself
            if args.inject:
                entries = []
                for i in range(1, args.inject + 1):
                    entries.append(P.TRAFFIC_ENTRY.pack(
                        i, P.pad(f"AI{i:03d}", 16), P.pad("A320", 8), lat, lon + 0.01 * i, alt_m + 100.0 * i,
                        args.heading, 0.0, 0.0, gs_ms, 1.0, 0.0, 0, 0, 0, P.XPDR_ALT,
                        int(time.monotonic() * 1000) & 0xFFFFFFFF, args.heading, 0.0, P.pad("", 16),
                        2000 + i, 0, 0))
                    if len(entries) == P.MAX_TRAFFIC_ENTRIES or i == args.inject:
                        sock.sendto(P.pack(P.PT_AI_TRAFFIC, sid, P.TRAFFIC_HDR.pack(len(entries), 0)
                                           + b"".join(entries)), dest)
                        entries = []
            # v6: a station publishes its ATIS every few seconds
            if args.atis and args.atc and time.time() >= next_text - 2.0:
                sock.sendto(P.pack(P.PT_ATIS, sid, P.atis(args.atis_freq, args.callsign.split("_")[0],
                                                          args.atis_letter, args.atis)), dest)

            # Text names its own frequency and does not need the PTT held.
            if args.talk and time.time() >= next_text:
                msg = f"{args.callsign} position report".encode()
                sock.sendto(P.pack(P.PT_TEXT, sid, P.TEXT_HDR.pack(
                    args.com1, sid, P.pad(args.callsign, 16), len(msg), P.pad("", 16)) + msg), dest)
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
    # v6
    ap.add_argument("--com2", type=int, default=0, help="kHz on COM2 (e.g. an ATIS frequency)")
    ap.add_argument("--atc", action="store_true",
                    help="log in as an ATC station (callsign like EETN_TWR, listed with COM1 as its frequency)")
    ap.add_argument("--atis", default="", help="with --atc: publish this ATIS text")
    ap.add_argument("--ack-plans", type=int, default=0, metavar="SQUAWK",
                    help="with --atc: acknowledge every filed flight plan with this squawk")
    ap.add_argument("--atis-freq", type=int, default=127905)
    ap.add_argument("--atis-letter", default="A")
    ap.add_argument("--inject", type=int, default=0,
                    help="log in as an injector driving this many computer-flown aircraft")
    ap.add_argument("--file-plan", default="", metavar="'DEP DEST ROUTE'",
                    help="file a flight plan after logging in, e.g. 'EETN EFHK DCT'")
    ap.add_argument("--rules", default="V", help="flight plan rules: I, V, Y or Z")
    ap.add_argument("--cruise", type=int, default=3500, help="flight plan cruise altitude, ft")
    ap.add_argument("--remarks", default="")
    ap.add_argument("--direct", default="", metavar="'CALLSIGN message'",
                    help="send one direct message after logging in")
    run(ap.parse_args())
