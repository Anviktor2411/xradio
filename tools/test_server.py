#!/usr/bin/env python3
"""Protocol and routing tests for the XRadio relay server.

Starts a real server on a throwaway port and drives it over real UDP, so this
covers the wire format as well as the routing logic.

    python3 tools/test_server.py
"""

import asyncio
import socket
import struct
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "server"))

import protocol as P          # noqa: E402
import server as S            # noqa: E402

PORT = 49317
FREQ = 122800                 # 122.800 MHz
OTHER_FREQ = 118100

failures = []


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f"  [{detail}]" if detail and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    """A minimal XRadio client for testing."""

    def __init__(self, callsign, lat, lon, alt_ft=3000.0, com1=FREQ, com2=0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.4)
        self.dest = ("127.0.0.1", PORT)
        self.callsign = callsign
        self.lat, self.lon, self.alt_ft = lat, lon, alt_ft
        self.com1, self.com2 = com1, com2
        self.sid = 0

    def login(self):
        self.sock.sendto(P.pack(P.PT_LOGIN, 0, P.login(self.callsign, "C172")), self.dest)
        pkt = self.recv(P.PT_LOGIN_ACK)
        if pkt is not None:
            self.sid, _ = P.LOGIN_ACK.unpack_from(pkt, 0)
        return self.sid

    def position(self, tx=P.TX_NONE, rx=P.RX_COM1, sid=None,
                 squawk=1200, xpdr=P.XPDR_ALT, ident=0):
        self.sock.sendto(P.pack(P.PT_POSITION, self.sid if sid is None else sid,
                                P.POSITION.pack(
                                    self.lat, self.lon, self.alt_ft / 3.28084,
                                    90.0, 0.0, 0.0, 60.0, 0.0, 0.0,
                                    self.com1, self.com2, 0, 0, tx, rx,
                                    int(time.monotonic() * 1000) & 0xFFFFFFFF, 90.0, 0.0,
                                    squawk, xpdr, ident)),
                         self.dest)

    def text(self, msg, freq, sid=None):
        b = msg.encode()
        self.sock.sendto(P.pack(P.PT_TEXT, self.sid if sid is None else sid,
                                P.TEXT_HDR.pack(freq, self.sid, P.pad(self.callsign, 16),
                                                len(b), P.pad("", 16)) + b), self.dest)

    def recv(self, want_type, timeout=0.6):
        """Read packets until one of want_type arrives, or give up."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.sock.settimeout(max(0.01, deadline - time.time()))
            try:
                data, _ = self.sock.recvfrom(P.MAX_PACKET)
            except socket.timeout:
                return None
            parsed = P.unpack_header(data)
            if parsed and parsed[0] == want_type:
                return parsed[4]
        return None

    def traffic_callsigns(self):
        payload = self.recv(P.PT_TRAFFIC)
        if payload is None:
            return None
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        out = []
        off = P.TRAFFIC_HDR.size
        for _ in range(count):
            e = P.TRAFFIC_ENTRY.unpack_from(payload, off)
            off += P.TRAFFIC_ENTRY.size
            out.append(P.cstr(e[1]))
        return out

    def close(self):
        self.sock.close()


def start_server():
    """Run the real server on its own thread so blocking client sockets in the
    test cannot starve its event loop."""
    box = {}
    ready = threading.Event()

    def main():
        async def go():
            loop = asyncio.get_running_loop()
            srv = S.XRadioServer()
            transport, _ = await loop.create_datagram_endpoint(
                lambda: srv, local_addr=("127.0.0.1", PORT))
            S.disable_udp_reset(transport)
            box["srv"] = srv
            box["transport"] = transport
            ready.set()
            await srv.run_traffic_loop()
        try:
            asyncio.run(go())
        except (asyncio.CancelledError, RuntimeError):
            pass

    t = threading.Thread(target=main, daemon=True)
    t.start()
    if not ready.wait(5.0):
        raise RuntimeError("server did not start")
    return box["srv"]


def run():
    srv = start_server()
    pump = time.sleep     # the server runs elsewhere; we just need to wait

    print("\nwire format")
    check("C++/Python struct sizes agree", all([
        P.HEADER.size == 12, P.LOGIN.size == 76, P.POSITION.size == 72,
        P.TRAFFIC_ENTRY.size == 108, P.TEXT_HDR.size == 42, P.VOICE_HDR.size == 12,
    ]))
    check("bad magic is rejected",
          P.unpack_header(struct.pack("<IBBHI", 0xDEADBEEF, 1, 1, 0, 0)) is None)
    check("truncated packet is rejected", P.unpack_header(b"\x58\x43") is None)
    check("payload shorter than declared is rejected",
          P.unpack_header(P.HEADER.pack(P.MAGIC, P.PT_TEXT, 1, 50, 0) + b"x") is None)

    a = Client("ESNA12", 57.8500, 27.0200)
    b = Client("ESNB34", 57.8800, 27.0500)          # ~2 nm away, same freq
    far = Client("ESFAR1", 62.0000, 27.0000, com1=FREQ)   # ~250 nm: beyond VHF
    mid = Client("ESMID1", 59.5000, 27.0000, com1=FREQ)   # ~100 nm: too far to
                                                          # see, close enough to hear
    off = Client("ESOFF1", 57.8600, 27.0300, com1=OTHER_FREQ)  # in range, wrong freq

    print("\nlogin")
    for c in (a, b, far, off, mid):
        pump(0.05)
        c.login()
    check("all clients got a session id", all(c.sid for c in (a, b, far, off, mid)),
          f"{[c.sid for c in (a,b,far,off,mid)]}")
    check("session ids are unique", len({c.sid for c in (a, b, far, off, mid)}) == 5)

    print("\ntraffic broadcast")
    for c in (a, b, far, off, mid):
        c.position()
    pump(0.6)
    seen = a.traffic_callsigns()
    check("A sees nearby traffic", seen is not None and "ESNB34" in seen, str(seen))
    check("A does not see itself", seen is not None and "ESNA12" not in seen, str(seen))
    check("traffic beyond 80 nm is filtered out",
          seen is not None and "ESFAR1" not in seen, str(seen))

    print("\nv2 fields are relayed untouched")
    stamp = 123456789
    while b.recv(P.PT_TRAFFIC, 0.05) is not None:   # drain the 10 Hz backlog first
        pass
    a.sock.sendto(P.pack(P.PT_POSITION, a.sid, P.POSITION.pack(
        a.lat, a.lon, a.alt_ft / 3.28084, 90.0, 0.0, 0.0, 60.0, 0.0, 0.0,
        a.com1, 0, 0, 0, P.TX_NONE, P.RX_COM1, stamp, 123.5, -2.5,
        1200, P.XPDR_ALT, 0)), a.dest)
    pump(0.4)
    relayed = None
    for _ in range(30):
        payload = b.recv(P.PT_TRAFFIC)
        if payload is None:
            break
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        # NB: not `off` -- that is the name of one of the clients above.
        pos = P.TRAFFIC_HDR.size
        for _ in range(count):
            e = P.TRAFFIC_ENTRY.unpack_from(payload, pos)
            pos += P.TRAFFIC_ENTRY.size
            if P.cstr(e[1]) == "ESNA12":
                relayed = e
        if relayed and relayed[16] == stamp:
            break
    check("sender timestamp arrives at the peer unchanged",
          relayed is not None and relayed[16] == stamp,
          str(relayed[16] if relayed else None))
    check("track and vertical speed arrive too",
          relayed is not None and abs(relayed[17] - 123.5) < 0.01 and abs(relayed[18] + 2.5) < 0.01)

    print("\ntext routing")
    for c in (a, b, far, off, mid):
        c.position()
    pump(0.3)
    a.text("radio check", FREQ)
    pump(0.3)
    check("B on the same frequency receives it", b.recv(P.PT_TEXT) is not None)
    check("wrong frequency receives nothing", off.recv(P.PT_TEXT, 0.3) is None)
    check("out of VHF range receives nothing", far.recv(P.PT_TEXT, 0.3) is None)
    # Radio horizon (~135 nm at 3000 ft) is deliberately longer than the 80 nm
    # traffic radius, so you can hear someone you cannot see -- as in real life.
    check("audible but not visible traffic still hears the call",
          mid.recv(P.PT_TEXT, 0.4) is not None)

    print("\ntext does not require the PTT")
    # a.position() above sent TX_NONE, and the message above still arrived.
    a.text("second call", FREQ)
    pump(0.3)
    check("still delivered with PTT released", b.recv(P.PT_TEXT) is not None)

    print("\nfrequency spoofing")
    a.text("should not arrive", OTHER_FREQ)   # A is not tuned to OTHER_FREQ
    pump(0.3)
    check("cannot transmit on a frequency you are not tuned to",
          off.recv(P.PT_TEXT, 0.3) is None)

    print("\nsession id check")
    before = len(srv.sessions)
    a.text("forged", FREQ, sid=99999)
    pump(0.3)
    check("packet with a wrong session id is dropped", b.recv(P.PT_TEXT, 0.3) is None)
    check("server still healthy", len(srv.sessions) == before)

    print("\ntext length cap")
    for c in (a, b):
        c.position()
    pump(0.3)
    a.text("x" * 500, FREQ)
    pump(0.3)
    payload = b.recv(P.PT_TEXT)
    if payload is None:
        check("oversized text still delivered, truncated", False, "nothing arrived")
    else:
        _f, _s, _frm, tlen, _to = P.TEXT_HDR.unpack_from(payload, 0)
        check("oversized text is capped", tlen <= S.MAX_TEXT_BYTES, f"len={tlen}")

    print("\nhostile client input")
    import struct as _st
    bad = Client("ESBAD1", 57.85, 27.02)
    bad.login()
    # a valid report first, so we can prove the bad ones are ignored not applied
    bad.position()
    pump(0.3)
    good_lat = next((x.lat for x in srv.sessions.values() if x.callsign == "ESBAD1"), None)
    for label, lat, lon, alt, gs in [
        ("NaN latitude", float("nan"), 27.0, 900.0, 60.0),
        ("infinite longitude", 57.0, float("inf"), 900.0, 60.0),
        ("latitude 500", 500.0, 27.0, 900.0, 60.0),
        ("altitude 1e9", 57.0, 27.0, 1e9, 60.0),
        ("negative ground speed", 57.0, 27.0, 900.0, -5.0),
    ]:
        bad.sock.sendto(P.pack(P.PT_POSITION, bad.sid, P.POSITION.pack(
            lat, lon, alt, 90.0, 0.0, 0.0, gs, 0.0, 0.0,
            FREQ, 0, 0, 0, P.TX_NONE, P.RX_COM1, 0, 90.0, 0.0,
            1200, P.XPDR_ALT, 0)), bad.dest)
    pump(0.4)
    now_lat = next((x.lat for x in srv.sessions.values() if x.callsign == "ESBAD1"), None)
    check("implausible position reports are rejected",
          now_lat is not None and now_lat == good_lat, f"{good_lat} -> {now_lat}")
    check("server survived them", len(srv.sessions) > 0)

    # gear/flap outside 0..1 get clamped rather than relayed raw
    bad.sock.sendto(P.pack(P.PT_POSITION, bad.sid, P.POSITION.pack(
        57.85, 27.02, 900.0, 90.0, 0.0, 0.0, 60.0, 9.0, -4.0,
        FREQ, 0, 0, 0, P.TX_NONE, P.RX_COM1, 0, 90.0, 0.0,
        1200, P.XPDR_ALT, 0)), bad.dest)
    pump(0.3)
    sess = next((x for x in srv.sessions.values() if x.callsign == "ESBAD1"), None)
    check("gear and flap ratios are clamped to 0..1",
          sess is not None and sess.gear == 1.0 and sess.flap == 0.0,
          f"gear={getattr(sess,'gear',None)} flap={getattr(sess,'flap',None)}")

    print("\ncallsign sanitising")
    ctl = Client("ESCTL1", 57.85, 27.02)
    ctl.sock.sendto(P.pack(P.PT_LOGIN, 0, P.LOGIN.pack(
        b"AB\x07\x1b[31mCD\x00\x00\x00\x00\x00\x00\x00", P.pad("C172", 8),
        P.PROTO_VERSION, 0, P.pad("", 16), P.pad("", 32))), ctl.dest)
    pkt = ctl.recv(P.PT_LOGIN_ACK)
    pump(0.2)
    names = [x.callsign for x in srv.sessions.values()]
    dirty = [n for n in names if any(ord(c) < 32 for c in n)]
    check("control characters are stripped from callsigns", not dirty, str(dirty))
    ctl.close()

    print("\nradio range maths")
    g = S.Session(1, None, "A", "C172", 0, lat=0, lon=0, alt_m=0)
    h = S.Session(2, None, "B", "C172", 0, lat=0, lon=0, alt_m=0)
    check("ground-to-ground has a usable minimum", S.radio_range_nm(g, h) >= 15.0)
    g.alt_m = 10000 / 3.28084     # 10 000 ft
    check("range grows with altitude", S.radio_range_nm(g, h) > 100.0,
          f"{S.radio_range_nm(g, h):.0f} nm")
    n, s2 = S.Session(1, None, "", "", 0, lat=57.0, lon=27.0), \
            S.Session(2, None, "", "", 0, lat=58.0, lon=27.0)
    d = S.distance_nm(n, s2)
    check("1 degree of latitude is ~60 nm", 59.0 < d < 61.0, f"{d:.2f} nm")

    print("\ntimeout")
    stale = Client("ESOLD1", 57.85, 27.02)
    stale.login()
    stale.position()
    pump(0.3)
    had = any(x.callsign == "ESOLD1" for x in srv.sessions.values())
    for x in list(srv.sessions.values()):
        if x.callsign == "ESOLD1":
            x.last_seen -= S.SESSION_TIMEOUT_S + 1
    pump(0.5)
    still = any(x.callsign == "ESOLD1" for x in srv.sessions.values())
    check("stale session is reaped", had and not still)

    print("\nthe weather claim moves with a re-login")
    # A host whose login ack was lost, or who logs in again after a network
    # stall, logs in from the same address. The old session's claim on the
    # sky used to survive that, naming a session that no longer existed, so
    # nobody could share weather again for the rest of the flight.
    src = Client("ESWXS1", 57.85, 27.02)
    lst = Client("ESWXL1", 57.855, 27.025)

    def claim():
        src.sock.sendto(P.pack(P.PT_LOGIN, 0, P.LOGIN.pack(
            P.pad("ESWXS1", 16), P.pad("C172", 8), P.PROTO_VERSION,
            P.LF_WEATHER_SOURCE, P.pad("", 16), P.pad("", 32))), src.dest)
        pkt = src.recv(P.PT_LOGIN_ACK)
        src.sid = P.LOGIN_ACK.unpack_from(pkt, 0)[0] if pkt else 0
        return src.sid

    sky = P.WEATHER.pack(*([0, 43200.0, 180, 4.5, 99400.0, 7.0, 0.6, 0.0, 0.0, 3]
                           + [0.0] * (4 * P.CLOUD_LAYERS + 7 * P.AIR_LAYERS)))
    first = claim()
    lst.login()
    src.sock.sendto(P.pack(P.PT_WEATHER, src.sid, sky), src.dest)
    check("the source's sky is relayed", lst.recv(P.PT_WEATHER) is not None)
    second = claim()
    check("logging in again gives a new session", first and second and second != first,
          f"{first} -> {second}")
    check("and the claim moved to it", srv._weather_sid == second,
          f"claim held by {srv._weather_sid}, session is {second}")
    src.sock.sendto(P.pack(P.PT_WEATHER, src.sid, sky), src.dest)
    check("so the sky is still relayed", lst.recv(P.PT_WEATHER) is not None)
    src.close(); lst.close()

    print("\nv6: ATC stations, injected traffic, flight plans, ATIS")
    ATIS_FREQ = 127905

    def login_flags(c, callsign, ac, flags):
        c.sock.sendto(P.pack(P.PT_LOGIN, 0, P.login(callsign, ac, flags=flags)), c.dest)
        pkt = c.recv(P.PT_LOGIN_ACK)
        c.sid = P.LOGIN_ACK.unpack_from(pkt, 0)[0] if pkt else 0
        return c.sid

    def traffic_now(c, settle=0.35):
        """The callsigns in the most recent traffic packet: read for a fixed
        window (traffic never stops coming, so a gap-based drain never ends)."""
        pump(settle)
        deadline = time.time() + 0.3
        last = None
        while time.time() < deadline:
            payload = c.recv(P.PT_TRAFFIC, max(0.01, deadline - time.time()))
            if payload is None:
                break
            count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
            pos, names = P.TRAFFIC_HDR.size, {}
            for _ in range(count):
                e = P.TRAFFIC_ENTRY.unpack_from(payload, pos)
                pos += P.TRAFFIC_ENTRY.size
                names[P.cstr(e[1])] = e
            last = names
        return last

    twr = Client("EETN_TWR", 57.85, 27.02, alt_ft=100.0)
    inj = Client("TTRAIN_AI", 57.85, 27.02)
    check("an ATC station and an injector log in",
          login_flags(twr, "EETN_TWR", "ATC", P.LF_ATC) and login_flags(inj, "TTRAIN_AI", "ZZZZ", P.LF_INJECTOR))
    keep = [a, b, twr, inj]
    ai_set = []          # what the injector currently drives; alive() keeps it fresh

    def alive():
        """Sessions time out after 15 s of silence and injected aircraft after
        5 s without a refresh; the sections below take longer than that."""
        for c in keep:
            c.position()
        if ai_set:
            inject(ai_set)
        pump(0.2)

    def traffic_first(c, settle=0.1):
        """The next traffic packet after a short settle (for flags that only hold for 0.4 s)."""
        pump(settle)
        payload = c.recv(P.PT_TRAFFIC, 0.5)
        if payload is None:
            return None
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        pos, names = P.TRAFFIC_HDR.size, {}
        for _ in range(count):
            e = P.TRAFFIC_ENTRY.unpack_from(payload, pos)
            pos += P.TRAFFIC_ENTRY.size
            names[P.cstr(e[1])] = e
        return names

    def atc_list_now(c):
        """The newest ATC list: it goes out on every change and every 5 s, and
        the client's queue may still hold older ones, so wait for one and then
        keep reading for a moment to get the latest."""
        lst = c.recv(P.PT_ATC_LIST, 6.0)
        deadline = time.time() + 0.3
        while lst is not None and time.time() < deadline:
            nxt = c.recv(P.PT_ATC_LIST, max(0.01, deadline - time.time()))
            if nxt is None:
                break
            lst = nxt
        out = {}
        if lst is not None:
            count, _ = P.ATC_LIST_HDR.unpack_from(lst, 0)
            pos = P.ATC_LIST_HDR.size
            for _ in range(count):
                e = P.ATC_ENTRY.unpack_from(lst, pos)
                pos += P.ATC_ENTRY.size
                out[P.cstr(e[1])] = e
        return out

    alive()
    seen = traffic_now(a) or {}
    check("the station is not relayed as traffic", "EETN_TWR" not in seen and "ESNB34" in seen, str(list(seen)))

    def ai_entry(local, cs, lat, lon, flags=0, squawk=4521):
        return P.TRAFFIC_ENTRY.pack(local, P.pad(cs, 16), P.pad("A320", 8), lat, lon, 500.0, 90.0, 0.0, 0.0,
                                    70.0, 1.0, 0.0, 0, 0, 0, P.XPDR_ALT, 0, 90.0, 0.0, P.pad("", 16), squawk, 0, flags)

    def inject(entries):
        inj.sock.sendto(P.pack(P.PT_AI_TRAFFIC, inj.sid, P.TRAFFIC_HDR.pack(len(entries), 0) + b"".join(entries)),
                        inj.dest)

    ai_set[:] = [ai_entry(1, "FIN321", 57.86, 27.03), ai_entry(2, "BTI101", 57.87, 27.04)]
    inject(ai_set)
    seen = traffic_now(a) or {}
    check("injected aircraft reach a pilot as traffic", "FIN321" in seen and "BTI101" in seen, str(list(seen)))
    check("with the injector's squawk and type", "FIN321" in seen and seen["FIN321"][20] == 4521 and
          P.cstr(seen["FIN321"][2]) == "A320")
    check("under a flight-wide id", "FIN321" in seen and seen["FIN321"][0] == P.ai_sid(inj.sid, 1),
          str(seen.get("FIN321", [None])[0]))
    seen_t = traffic_now(twr) or {}
    check("the station sees pilots and injected aircraft", "ESNA12" in seen_t and "FIN321" in seen_t, str(list(seen_t)))
    inj.position()
    seen_i = traffic_now(inj) or {}
    check("the injector does not get its own aircraft back", "FIN321" not in seen_i and "ESNA12" in seen_i, str(list(seen_i)))

    stations = atc_list_now(twr)
    check("the ATC list names the station with its frequency and position",
          "EETN_TWR" in stations and stations["EETN_TWR"][3] == FREQ and stations["EETN_TWR"][4] == P.ATC_TWR
          and P.cstr(stations["EETN_TWR"][2]) == "EETN", str({k: (v[3], v[4]) for k, v in stations.items()}))
    check("the injector is not in it", "TTRAIN_AI" not in stations)

    print("\nv6: an injector speaks as its aircraft")
    alive()
    aisid = P.ai_sid(inj.sid, 1)
    msg = b"Tallinn Tower, Finnair 321, established"
    inj.sock.sendto(P.pack(P.PT_TEXT, inj.sid, P.TEXT_HDR.pack(FREQ, aisid, P.pad("FIN321", 16), len(msg),
                                                              P.pad("", 16)) + msg), inj.dest)
    pump(0.3)
    got = b.recv(P.PT_TEXT)
    hdr = P.TEXT_HDR.unpack_from(got, 0) if got else None
    check("text from an injected aircraft reaches a pilot on the frequency, as that aircraft",
          hdr is not None and P.cstr(hdr[2]) == "FIN321" and hdr[1] == aisid, str(hdr))
    check("and the station hears it too", twr.recv(P.PT_TEXT) is not None)
    check("but not the injector itself", inj.recv(P.PT_TEXT, 0.3) is None)
    inj.sock.sendto(P.pack(P.PT_VOICE, inj.sid, P.VOICE_HDR.pack(FREQ, aisid, 1, 4) + b"\x01\x02\x03\x04"), inj.dest)
    pump(0.2)
    v = b.recv(P.PT_VOICE)
    check("voice from an injected aircraft is relayed as that aircraft", v is not None and P.VOICE_HDR.unpack_from(v, 0)[1] == aisid)
    seen = traffic_first(b) or {}
    check("and the aircraft shows as transmitting", "FIN321" in seen and seen["FIN321"][14] == 1,
          str(seen.get("FIN321", [None] * 15)[14]))
    other = Client("ESOTH1", 57.85, 27.02, com1=OTHER_FREQ)
    other.login(); other.position()
    pump(0.3)
    msg2 = b"wrong frequency"
    inj.sock.sendto(P.pack(P.PT_TEXT, inj.sid, P.TEXT_HDR.pack(OTHER_FREQ, P.ai_sid(inj.sid, 9), P.pad("GHOST", 16),
                                                              len(msg2), P.pad("", 16)) + msg2), inj.dest)
    pump(0.3)
    check("an id the injector does not drive is treated as the injector itself (not tuned: dropped)",
          other.recv(P.PT_TEXT, 0.3) is None)
    other.close()

    print("\nv6: flight plans")
    alive()
    a.sock.sendto(P.pack(P.PT_FLIGHTPLAN, a.sid, P.flightplan("SPOOF1", "C172", "EETN", "EFHK", "DCT", rules="V",
                                                             cruise_ft=3500, squawk=7777)), a.dest)
    fp = twr.recv(P.PT_FLIGHTPLAN)
    d = P.unpack_flightplan(fp) if fp else None
    check("a filed plan reaches the station with the pilot's session and callsign",
          d is not None and d["session_id"] == a.sid and d["callsign"] == "ESNA12" and d["dest"] == "EFHK", str(d))
    check("a pilot cannot assign their own squawk", d is not None and d["squawk"] == 0)
    twr.sock.sendto(P.pack(P.PT_FLIGHTPLAN, twr.sid, P.flightplan("ESNA12", "C172", "EETN", "EFHK", "DCT",
                                                                 status=P.FP_ACK, squawk=4521, session_id=a.sid,
                                                                 remarks="cleared to EFHK via DCT")), twr.dest)
    ack = a.recv(P.PT_FLIGHTPLAN)
    d2 = P.unpack_flightplan(ack) if ack else None
    check("the station's answer reaches the pilot with the squawk",
          d2 is not None and d2["status"] == P.FP_ACK and d2["squawk"] == 4521 and d2["remarks"].startswith("cleared"), str(d2))
    check("other pilots do not see it", b.recv(P.PT_FLIGHTPLAN, 0.3) is None)
    gnd = Client("EETN_GND", 57.85, 27.02, alt_ft=100.0, com1=OTHER_FREQ)
    login_flags(gnd, "EETN_GND", "ATC", P.LF_ATC)
    keep.append(gnd)
    late = gnd.recv(P.PT_FLIGHTPLAN)
    d3 = P.unpack_flightplan(late) if late else None
    check("a station that joins later gets the plans on login, as last answered",
          d3 is not None and d3["callsign"] == "ESNA12" and d3["squawk"] == 4521, str(d3))
    inj.sock.sendto(P.pack(P.PT_FLIGHTPLAN, inj.sid, P.flightplan("TTRAIN_AI", "ZZZZ", "EETN", "EFHK")), inj.dest)
    pump(0.3)
    check("an injector cannot file", twr.recv(P.PT_FLIGHTPLAN, 0.3) is None)

    print("\nv6: ATIS")
    alive()
    twr.sock.sendto(P.pack(P.PT_ATIS, twr.sid, P.atis(ATIS_FREQ, "EETN", "C", "Tallinn information Charlie, runway 26")), twr.dest)
    pump(0.3)
    b.com2 = ATIS_FREQ
    b.position(rx=P.RX_COM1 | P.RX_COM2)
    at = b.recv(P.PT_ATIS)
    ah = P.ATIS_HDR.unpack_from(at, 0) if at else None
    check("a pilot who tunes the ATIS frequency gets the text",
          ah is not None and P.cstr(ah[1]) == "EETN" and ah[2] == b"C" and b"Charlie" in at[P.ATIS_HDR.size:], str(ah))
    check("a pilot who is not tuned gets nothing", a.recv(P.PT_ATIS, 0.3) is None)
    b.position(rx=P.RX_COM1 | P.RX_COM2)
    check("the same letter is not sent again on every position report", b.recv(P.PT_ATIS, 0.4) is None)
    twr.sock.sendto(P.pack(P.PT_ATIS, twr.sid, P.atis(ATIS_FREQ, "EETN", "D", "Tallinn information Delta, runway 08")), twr.dest)
    at2 = b.recv(P.PT_ATIS)
    check("a new letter goes out to those tuned at once", at2 is not None and P.ATIS_HDR.unpack_from(at2, 0)[2] == b"D")
    names = [(k, v[3], v[4]) for k, v in atc_list_now(gnd).items()]
    check("the ATIS frequency is in the ATC list", ("EETN_ATIS", ATIS_FREQ, P.ATC_ATIS) in names, str(names))
    a.sock.sendto(P.pack(P.PT_ATIS, a.sid, P.atis(ATIS_FREQ, "EETN", "Z", "not from a station")), a.dest)
    pump(0.3)
    check("only a station may publish an ATIS", srv.atis["EETN"]["letter"] == "D")

    print("\nv6: injected aircraft go away")
    alive()
    ai_set[:] = [ai_entry(1, "FIN321", 57.86, 27.03)]
    inject([ai_entry(1, "FIN321", 57.86, 27.03), ai_entry(2, "BTI101", 57.87, 27.04, flags=P.AI_REMOVE)])
    seen = traffic_now(a) or {}
    check("an aircraft flagged removed is gone at once", "BTI101" not in seen and "FIN321" in seen, str(list(seen)))
    ai_set[:] = []
    for ai in srv.ai.values():
        ai.last_seen -= S.AI_TIMEOUT_S + 1
    seen = traffic_now(a, 0.5) or {}
    check("one the injector stopped refreshing is dropped", "FIN321" not in seen, str(list(seen)))
    inject([ai_entry(3, "RYR9KL", 57.86, 27.03)])
    pump(0.3)
    check("the injector can add more", any(x.callsign == "RYR9KL" for x in srv.ai.values()))
    inj.sock.sendto(P.pack(P.PT_LOGOUT, inj.sid), inj.dest)
    pump(0.3)
    check("an injector that leaves takes its aircraft with it", not srv.ai)
    twr.sock.sendto(P.pack(P.PT_LOGOUT, twr.sid), twr.dest)
    pump(0.3)
    check("a station that leaves takes its ATIS with it", "EETN" not in srv.atis)
    for c in (twr, inj, gnd):
        c.close()

    for c in (a, b, far, off, mid, stale, bad):
        c.close()


if __name__ == "__main__":
    try:
        run()
    except KeyboardInterrupt:
        pass
    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        sys.exit(1)
    print("all tests passed")
