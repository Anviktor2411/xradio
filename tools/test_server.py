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
        self.sock.sendto(P.pack(P.PT_LOGIN, 0, P.LOGIN.pack(
            P.pad(self.callsign, 16), P.pad("C172", 8), P.PROTO_VERSION, 0)), self.dest)
        pkt = self.recv(P.PT_LOGIN_ACK)
        if pkt is not None:
            self.sid, _ = P.LOGIN_ACK.unpack_from(pkt, 0)
        return self.sid

    def position(self, tx=P.TX_NONE, rx=P.RX_COM1, sid=None):
        self.sock.sendto(P.pack(P.PT_POSITION, self.sid if sid is None else sid,
                                P.POSITION.pack(
                                    self.lat, self.lon, self.alt_ft / 3.28084,
                                    90.0, 0.0, 0.0, 60.0, 0.0, 0.0,
                                    self.com1, self.com2, 0, 0, tx, rx)), self.dest)

    def text(self, msg, freq, sid=None):
        b = msg.encode()
        self.sock.sendto(P.pack(P.PT_TEXT, self.sid if sid is None else sid,
                                P.TEXT_HDR.pack(freq, self.sid, P.pad(self.callsign, 16),
                                                len(b)) + b), self.dest)

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
        P.HEADER.size == 12, P.LOGIN.size == 28, P.POSITION.size == 56,
        P.TRAFFIC_ENTRY.size == 76, P.TEXT_HDR.size == 26, P.VOICE_HDR.size == 12,
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
        _f, _s, _frm, tlen = P.TEXT_HDR.unpack_from(payload, 0)
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
            FREQ, 0, 0, 0, P.TX_NONE, P.RX_COM1)), bad.dest)
    pump(0.4)
    now_lat = next((x.lat for x in srv.sessions.values() if x.callsign == "ESBAD1"), None)
    check("implausible position reports are rejected",
          now_lat is not None and now_lat == good_lat, f"{good_lat} -> {now_lat}")
    check("server survived them", len(srv.sessions) > 0)

    # gear/flap outside 0..1 get clamped rather than relayed raw
    bad.sock.sendto(P.pack(P.PT_POSITION, bad.sid, P.POSITION.pack(
        57.85, 27.02, 900.0, 90.0, 0.0, 0.0, 60.0, 9.0, -4.0,
        FREQ, 0, 0, 0, P.TX_NONE, P.RX_COM1)), bad.dest)
    pump(0.3)
    sess = next((x for x in srv.sessions.values() if x.callsign == "ESBAD1"), None)
    check("gear and flap ratios are clamped to 0..1",
          sess is not None and sess.gear == 1.0 and sess.flap == 0.0,
          f"gear={getattr(sess,'gear',None)} flap={getattr(sess,'flap',None)}")

    print("\ncallsign sanitising")
    ctl = Client("ESCTL1", 57.85, 27.02)
    ctl.sock.sendto(P.pack(P.PT_LOGIN, 0, P.LOGIN.pack(
        b"AB\x07\x1b[31mCD\x00\x00\x00\x00\x00\x00\x00", P.pad("C172", 8),
        P.PROTO_VERSION, 0)), ctl.dest)
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
