#!/usr/bin/env python3
"""Differential test: the Python server and the built-in C++ server must behave
identically.

There are two relay servers now -- `server/server.py` for a machine that
should stay up without X-Plane, and the one compiled into the plugin so a
pilot can just switch Hosting on. Two implementations of one protocol is
exactly the situation where they drift apart: someone fixes a validation rule
in one and forgets the other, and the bug only shows up when the two halves of
a flight are hosted differently.

So this runs the same black-box scenarios against both over real UDP and
compares the transcripts byte for byte. It also asserts what the answers
should actually be, so "both are equally wrong" still fails.

    python3 tools/test_server_parity.py [--cpp ./build-tests/xradio_server]
"""

import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "server"))

import protocol as P          # noqa: E402

FREQ = 122800                 # 122.800 MHz
OTHER_FREQ = 118100

failures = []


def check(name, cond, detail=""):
    print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f"  [{detail}]" if detail and not cond else ""))
    if not cond:
        failures.append(name)


class Client:
    def __init__(self, port, callsign, lat, lon, alt_ft=3000.0, com1=FREQ, com2=0,
                 ac="C172"):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.25)
        self.dest = ("127.0.0.1", port)
        self.callsign = callsign
        self.ac = ac
        self.lat, self.lon, self.alt_ft = lat, lon, alt_ft
        self.com1, self.com2 = com1, com2
        self.sid = 0

    def send(self, ptype, payload=b"", sid=None):
        self.sock.sendto(P.pack(ptype, self.sid if sid is None else sid, payload),
                         self.dest)

    def login(self, proto=P.PROTO_VERSION, callsign=None, ac=None, livery="",
              password=""):
        self.send(P.PT_LOGIN, P.LOGIN.pack(
            P.pad(callsign if callsign is not None else self.callsign, 16),
            P.pad(ac if ac is not None else self.ac, 8), proto, 0,
            P.pad(livery, 16), P.pad(password, 32)), sid=0)
        self.rejected = None
        pkt = self.recv(None)
        while pkt is not None:
            ptype, payload = pkt
            if ptype == P.PT_LOGIN_ACK:
                self.sid, _ = P.LOGIN_ACK.unpack_from(payload, 0)
                return self.sid
            if ptype == P.PT_LOGIN_REJECT:
                self.rejected, _ = P.LOGIN_REJECT.unpack_from(payload, 0)
                return 0
            pkt = self.recv(None)
        return 0

    def position(self, tx=P.TX_NONE, rx=P.RX_COM1, sid=None, lat=None, lon=None,
                 alt_ft=None, gs=50.0, gear=0.0, flap=0.0, time_ms=0,
                 track=90.0, vs=0.0, heading=90.0,
                 squawk=1200, xpdr=P.XPDR_ALT, ident=0):
        self.send(P.PT_POSITION, P.POSITION.pack(
            self.lat if lat is None else lat,
            self.lon if lon is None else lon,
            (self.alt_ft if alt_ft is None else alt_ft) / 3.28084,
            heading, 0.0, 0.0, gs, gear, flap,
            self.com1, self.com2, 0, 0, tx, rx, time_ms, track, vs,
            squawk, xpdr, ident), sid=sid)

    def text(self, body, freq=0, sid=None):
        raw = body.encode()
        self.send(P.PT_TEXT,
                  P.TEXT_HDR.pack(freq, 0, P.pad(self.callsign, 16), len(raw)) + raw,
                  sid=sid)

    def voice(self, seq=1, freq=0, opus=b"\x01\x02\x03\x04"):
        self.send(P.PT_VOICE, P.VOICE_HDR.pack(freq, 0, seq, len(opus)) + opus)

    def recv(self, want=None, timeout=0.25):
        end = time.time() + timeout
        while time.time() < end:
            self.sock.settimeout(max(0.01, end - time.time()))
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                return None
            parsed = P.unpack_header(data)
            if parsed is None:
                continue
            ptype, _v, _l, _sid, payload = parsed
            if want is None:
                return (ptype, payload)
            if ptype == want:
                return payload
        return None

    def drain(self, seconds):
        """Collect everything that arrives, grouped by packet type."""
        out = {P.PT_TRAFFIC: [], P.PT_TEXT: [], P.PT_VOICE: [], P.PT_PONG: [],
               P.PT_WEATHER: []}
        end = time.time() + seconds
        while time.time() < end:
            self.sock.settimeout(max(0.01, end - time.time()))
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                break
            parsed = P.unpack_header(data)
            if parsed is None:
                continue
            ptype, _v, _l, _sid, payload = parsed
            out.setdefault(ptype, []).append(payload)
        return out

    def close(self):
        self.sock.close()


def norm(callsign):
    """A placeholder callsign carries the session number, which depends on how
    many clients logged in earlier. Fold it away so the comparison is about
    behaviour, not ordering."""
    return "UNK*" if re.fullmatch(r"UNK\d+", callsign) else callsign


def wait_traffic(c, settle=0.3, cap=3.0, want=None):
    """Traffic packets, waited for rather than sampled.

    The servers broadcast on their own clock (10 Hz), so a fixed window can
    miss a cycle, or catch only one sent before the aircraft's first position
    was processed -- and then the two transcripts differ for no reason but
    timing, which is exactly what this test must not report as a difference.
    `want` is a callsign prefix that has to appear before we stop waiting;
    without it, the first packet of any kind is enough. Either way the window
    then stays open for `settle` to catch the rest of the cycle, and a wait
    that times out returns what the fixed window would have: nothing."""
    packets = []
    end = time.time() + cap
    while time.time() < end:
        packets += c.drain(0.25)[P.PT_TRAFFIC]
        if traffic_set(packets, (want,)) if want else packets:
            break
    packets += c.drain(settle)[P.PT_TRAFFIC]
    return packets


def traffic_set(packets, keep=None):
    """Every aircraft seen across a burst of traffic packets, as a stable set.

    `keep` limits it to this scenario's own aircraft: sessions live for 15
    seconds, so without it each scenario would also see the leftovers of the
    ones before and the result would depend on how fast the suite ran."""
    seen = set()
    for payload in packets:
        if len(payload) < P.TRAFFIC_HDR.size:
            continue
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        off = P.TRAFFIC_HDR.size
        for _ in range(count):
            if off + P.TRAFFIC_ENTRY.size > len(payload):
                break
            e = P.TRAFFIC_ENTRY.unpack_from(payload, off)
            off += P.TRAFFIC_ENTRY.size
            cs = norm(P.cstr(e[1]))
            if keep is not None and not cs.startswith(tuple(keep)):
                continue
            # lat, lon, alt, heading, ground speed, vertical speed and the
            # sender's timestamp: enough that a validation rule quietly
            # dropped from one server shows up as a different aircraft state.
            seen.add((cs, P.cstr(e[2]),
                      round(e[3], 4), round(e[4], 4), round(e[5], 1),
                      round(e[6], 1), round(e[9], 1), round(e[18], 1),
                      e[16], e[12], e[13]))
    return sorted(seen)


def tx_flags(packets, keep=None):
    """Whether any traffic entry was marked as transmitting, per callsign."""
    flags = {}
    for payload in packets:
        if len(payload) < P.TRAFFIC_HDR.size:
            continue
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        off = P.TRAFFIC_HDR.size
        for _ in range(count):
            if off + P.TRAFFIC_ENTRY.size > len(payload):
                break
            e = P.TRAFFIC_ENTRY.unpack_from(payload, off)
            off += P.TRAFFIC_ENTRY.size
            cs = norm(P.cstr(e[1]))
            if keep is not None and not cs.startswith(tuple(keep)):
                continue
            flags[cs] = flags.get(cs, 0) | int(e[14])
    return sorted(flags.items())


def texts(packets):
    out = []
    for payload in packets:
        if len(payload) < P.TEXT_HDR.size:
            continue
        freq, _sid, frm, n = P.TEXT_HDR.unpack_from(payload, 0)
        body = payload[P.TEXT_HDR.size:P.TEXT_HDR.size + n]
        out.append((freq, P.cstr(frm), body.decode("utf-8", "replace")))
    return sorted(out)


def voices(packets):
    out = []
    for payload in packets:
        if len(payload) < P.VOICE_HDR.size:
            continue
        freq, _sid, seq, n = P.VOICE_HDR.unpack_from(payload, 0)
        out.append((freq, seq, payload[P.VOICE_HDR.size:P.VOICE_HDR.size + n]))
    return sorted(out)


# ---------------------------------------------------------------------------
# the scenarios -- each returns a value that must match between the two servers
# ---------------------------------------------------------------------------
def scenario_login_and_traffic(port):
    a = Client(port, "ESNA12", 57.85, 27.02)
    b = Client(port, "ESNB34", 57.86, 27.03, ac="A20N")
    far = Client(port, "ESFAR1", 60.00, 27.02)          # ~130 nm north
    out = {}
    out["sid_a"] = a.login()
    out["sid_b"] = b.login()
    out["sid_far"] = far.login()
    for _ in range(3):
        a.position(); b.position(); far.position()
        time.sleep(0.12)
    mine = ("ESNA12", "ESNB34", "ESFAR1")
    out["a_sees"] = traffic_set(wait_traffic(a, want="ESNB34"), mine)
    out["far_sees"] = traffic_set(far.drain(0.4)[P.PT_TRAFFIC], mine)
    for c in (a, b, far):
        c.close()
    return out


def scenario_bad_login(port):
    out = {}
    v1 = Client(port, "OLDVER", 57.85, 27.02)
    out["old_protocol_gets_no_ack"] = (v1.login(proto=1) == 0)
    ctl = Client(port, "BAD\x07CS\x01", 57.85, 27.02)
    ctl.login()
    empty = Client(port, "   ", 57.85, 27.02, ac="")
    empty.login()
    peer = Client(port, "WATCH1", 57.85, 27.02)
    peer.login()
    for _ in range(3):
        ctl.position(); empty.position(); peer.position()
        time.sleep(0.12)
    mine = ("BADCS", "UNK*", "WATCH1")
    out["callsigns_seen"] = [t[0] for t in
                             traffic_set(wait_traffic(peer, want="WATCH1"), mine)]
    out["ac_types_seen"] = sorted({t[1] for t in
                                   traffic_set(wait_traffic(peer, want="WATCH1"), mine)})
    for c in (v1, ctl, empty, peer):
        c.close()
    return out


def scenario_position_validation(port):
    """Bad values must be dropped, leaving the last good position in place.

    Every rejected report carries a different timestamp, and the timestamp is
    relayed untouched, so if any one of these is wrongly accepted the watcher
    sees a state it should never have seen -- even when the bad field itself
    is not the one being looked at.
    """
    a = Client(port, "ESVAL1", 57.85, 27.02)
    b = Client(port, "ESWAT1", 57.851, 27.021)
    a.login(); b.login()
    a.position(lat=57.85, lon=27.02, gs=50.0, vs=0.0, time_ms=1)   # a good one first
    b.position()
    time.sleep(0.2)
    bad = [
        (float("nan"), 27.02, 3000, 50, 0),      # not-a-number latitude
        (57.85, float("inf"), 3000, 50, 0),      # infinite longitude
        (57.85, 27.02, float("nan"), 50, 0),     # not-a-number altitude
        (91.0, 27.02, 3000, 50, 0),              # off the top of the planet
        (-91.0, 27.02, 3000, 50, 0),
        (57.85, 181.0, 3000, 50, 0),
        (57.85, -181.0, 3000, 50, 0),
        (57.85, 27.02, 200000, 50, 0),           # 200 000 ft
        (57.85, 27.02, -5000, 50, 0),            # below the Dead Sea
        (57.85, 27.02, 3000, 9999, 0),           # 9999 m/s ground speed
        (57.85, 27.02, 3000, -10, 0),            # negative ground speed
        (57.85, 27.02, 3000, 50, 5000),          # 5000 m/s vertical
        (57.85, 27.02, 3000, 50, -5000),
        (57.85, 27.02, 3000, float("nan"), 0),   # not-a-number ground speed
        (57.85, 27.02, 3000, 50, float("nan")),  # not-a-number vertical speed
    ]
    for i, (lat, lon, alt, gs, vs) in enumerate(bad):
        a.position(lat=lat, lon=lon, alt_ft=alt, gs=gs, vs=vs, time_ms=1000 + i)
    for _ in range(3):
        b.position()
        time.sleep(0.12)
    out = {"b_sees": traffic_set(wait_traffic(b, want="ESVAL1"), ("ESVAL1",))}
    a.close(); b.close()
    return out


def scenario_text_routing(port):
    a = Client(port, "ESTXA1", 57.85, 27.02)
    same = Client(port, "ESTXB1", 57.855, 27.025)
    other = Client(port, "ESTXC1", 57.856, 27.026, com1=OTHER_FREQ)
    far = Client(port, "ESTXD1", 60.50, 27.02)      # past the VHF horizon
    for c in (a, same, other, far):
        c.login()
        c.position()
    time.sleep(0.2)
    a.text("hello all", freq=FREQ)
    a.text("wrong radio", freq=121500)              # not tuned there
    a.text("x" * 400, freq=FREQ)                    # over the 200-byte cap
    time.sleep(0.3)
    out = {
        "same_freq": texts(same.drain(0.3)[P.PT_TEXT]),
        "other_freq": texts(other.drain(0.2)[P.PT_TEXT]),
        "out_of_range": texts(far.drain(0.2)[P.PT_TEXT]),
    }
    for c in (a, same, other, far):
        c.close()
    return out


def scenario_voice_routing(port):
    a = Client(port, "ESVCA1", 57.85, 27.02)
    b = Client(port, "ESVCB1", 57.855, 27.025)
    off = Client(port, "ESVCC1", 57.856, 27.026, com1=OTHER_FREQ)
    for c in (a, b, off):
        c.login()
        c.position(tx=P.TX_COM1)
    time.sleep(0.2)
    for i in range(5):
        a.voice(seq=i, freq=FREQ)
        time.sleep(0.02)
    time.sleep(0.2)
    got = b.drain(0.3)
    out = {
        "heard": voices(got[P.PT_VOICE]),
        "not_heard": voices(off.drain(0.2)[P.PT_VOICE]),
    }
    # keep talking so the tx flag is definitely set while traffic goes out
    for i in range(6):
        a.voice(seq=100 + i, freq=FREQ)
        b.position(tx=P.TX_NONE)
        time.sleep(0.05)
    out["tx_flags"] = tx_flags(wait_traffic(b, want="ESVC"), ("ESVC",))
    for c in (a, b, off):
        c.close()
    return out


def scenario_session_id(port):
    a = Client(port, "ESSID1", 57.85, 27.02)
    b = Client(port, "ESSID2", 57.855, 27.025)
    a.login(); b.login()
    a.position(); b.position()
    time.sleep(0.2)
    # A position claiming somebody else's session id must be ignored.
    a.position(sid=a.sid + 500, lat=10.0, lon=10.0)
    a.text("spoofed", freq=FREQ, sid=a.sid + 500)
    for _ in range(3):
        b.position()
        time.sleep(0.12)
    got = b.drain(0.4)
    out = {"b_sees": traffic_set(got[P.PT_TRAFFIC], ("ESSID",)),
           "b_texts": texts(got[P.PT_TEXT])}
    # ping/pong still works with the right id
    a.send(P.PT_PING)
    out["pong"] = a.recv(P.PT_PONG) is not None
    a.close(); b.close()
    return out


def scenario_logout(port):
    a = Client(port, "ESOUT1", 57.85, 27.02)
    b = Client(port, "ESOUT2", 57.855, 27.025)
    a.login(); b.login()
    a.position(); b.position()
    time.sleep(0.25)
    before = traffic_set(wait_traffic(b, want="ESOUT1"), ("ESOUT",))
    a.send(P.PT_LOGOUT)
    time.sleep(0.25)
    for _ in range(2):
        b.position()
        time.sleep(0.12)
    after = traffic_set(wait_traffic(b), ("ESOUT",))
    a.close(); b.close()
    return {"before": [t[0] for t in before], "after": [t[0] for t in after]}


def scenario_garbage(port):
    """Nonsense must be ignored without disturbing a working session."""
    a = Client(port, "ESGAR1", 57.85, 27.02)
    b = Client(port, "ESGAR2", 57.855, 27.025)
    a.login(); b.login()
    junk = [
        b"",
        b"\x00" * 4,
        b"XRC1",
        P.HEADER.pack(0xDEADBEEF, P.PT_POSITION, P.PROTO_VERSION, 0, a.sid),
        P.HEADER.pack(P.MAGIC, P.PT_POSITION, 99, 0, a.sid),
        P.HEADER.pack(P.MAGIC, P.PT_POSITION, P.PROTO_VERSION, 9999, a.sid),
        P.HEADER.pack(P.MAGIC, P.PT_POSITION, P.PROTO_VERSION, 4, a.sid) + b"\x01\x02",
        P.HEADER.pack(P.MAGIC, 200, P.PROTO_VERSION, 0, a.sid),
        P.pack(P.PT_TEXT, a.sid, b"\x00" * 3),
        P.pack(P.PT_VOICE, a.sid, b"\x00" * 5),
        P.pack(P.PT_LOGIN, 0, b"short"),
        os.urandom(64),
    ]
    for j in junk:
        a.sock.sendto(j, a.dest)
    time.sleep(0.2)
    for _ in range(3):
        a.position(); b.position()
        time.sleep(0.12)
    out = {"still_working": traffic_set(wait_traffic(b, want="ESGAR"), ("ESGAR",))}
    a.close(); b.close()
    return out


def scenario_password(port):
    """The server runs with a flight password; the door must behave the same."""
    out = {}
    nopw = Client(port, "ESNOPW", 57.85, 27.02)
    out["without_password_sid"] = nopw.login()
    out["without_password_reason"] = nopw.rejected
    wrong = Client(port, "ESWRNG", 57.85, 27.02)
    out["wrong_password_sid"] = wrong.login(password="clouds")
    out["wrong_password_reason"] = wrong.rejected
    right = Client(port, "ESOKPW", 57.85, 27.02)
    out["right_password_joined"] = right.login(password="sky") != 0
    old = Client(port, "ESOLDV", 57.85, 27.02)
    old.login(proto=2, password="sky")
    out["old_version_reason"] = old.rejected
    # a v2 *header* gets the same answer, so an old build sees why
    old.sock.sendto(P.HEADER.pack(P.MAGIC, P.PT_LOGIN, 2, P.LOGIN.size,
                                  0) + P.login("ESOLDV", "C172", "", "sky"), old.dest)
    pkt = old.recv(P.PT_LOGIN_REJECT)
    out["old_header_reason"] = P.LOGIN_REJECT.unpack_from(pkt, 0)[0] if pkt else None
    # liveries come back in traffic
    a = Client(port, "ESLIVA", 57.85, 27.02)
    b = Client(port, "ESLIVB", 57.855, 27.025)
    a.login(password="sky", livery="Lufthansa"); b.login(password="sky")
    for _ in range(3):
        a.position(); b.position(); time.sleep(0.12)
    liveries = set()
    for payload in wait_traffic(b, want="ESLIVA"):
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        off = P.TRAFFIC_HDR.size
        for _ in range(count):
            e = P.TRAFFIC_ENTRY.unpack_from(payload, off); off += P.TRAFFIC_ENTRY.size
            if P.cstr(e[1]) == "ESLIVA":
                liveries.add(P.cstr(e[19]))
    out["livery_seen"] = sorted(liveries)
    for c in (nopw, wrong, right, old, a, b):
        c.close()
    return out


def scenario_weather(port):
    """One sim decides the sky. Both servers must relay it only from the
    pilot who claimed it, or a stranger could move everyone's weather."""
    out = {}
    src = Client(port, "ESWXA1", 57.85, 27.02)
    other = Client(port, "ESWXB1", 57.855, 27.025)

    # the source says so in its login
    src.send(P.PT_LOGIN, P.LOGIN.pack(P.pad("ESWXA1", 16), P.pad("C172", 8),
             P.PROTO_VERSION, P.LF_WEATHER_SOURCE, P.pad("", 16), P.pad("", 32)), sid=0)
    ack = src.recv(P.PT_LOGIN_ACK)
    src.sid = P.LOGIN_ACK.unpack_from(ack, 0)[0] if ack else 0
    out["source_got_in"] = src.sid != 0
    other.login()

    sky = [0, 43200.0, 180, 4.5, 99400.0, 7.0, 0.6, 0.0, 0.0, 3] + [0.0] * (4 * 3) + \
          [0.0] * (7 * P.AIR_LAYERS)
    sky[10] = 120.0                      # first cloud type, as a marker
    src.send(P.PT_WEATHER, P.WEATHER.pack(*sky))
    time.sleep(0.2)
    got = other.drain(0.4)[P.PT_WEATHER]
    out["relayed"] = len(got) > 0
    out["intact"] = [round(v, 3) for v in P.WEATHER.unpack(got[0])[1:6]] if got else []

    # a pilot who never claimed it cannot move the sky
    other.send(P.PT_WEATHER, P.WEATHER.pack(*sky))
    time.sleep(0.2)
    out["stranger_relayed"] = len(src.drain(0.4)[P.PT_WEATHER]) > 0

    # and a truncated one is not passed on either
    src.send(P.PT_WEATHER, b"\x01\x02\x03")
    time.sleep(0.2)
    out["runt_relayed"] = len(other.drain(0.3)[P.PT_WEATHER]) > 0

    src.close(); other.close()
    return out


def scenario_transponder(port):
    """The transponder is relayed as sent, except where it cannot be true.

    The receiving end decides what to do with a squawk -- whether the aircraft
    is on TCAS, whether it has an altitude to show -- so a server that quietly
    alters one, or passes on a code that no transponder could produce, breaks
    that decision on every client at once.
    """
    out = {}
    a = Client(port, "ESXPA1", 57.85, 27.02)
    b = Client(port, "ESXPB1", 57.851, 27.021)
    a.login(); b.login()

    def seen(sender, squawk, xpdr, ident, time_ms):
        sender.position(squawk=squawk, xpdr=xpdr, ident=ident, time_ms=time_ms)
        b.position(time_ms=time_ms)
        time.sleep(0.25)
        for raw in b.drain(0.5)[P.PT_TRAFFIC]:
            count, _ = P.TRAFFIC_HDR.unpack_from(raw, 0)
            off = P.TRAFFIC_HDR.size
            for _ in range(count):
                e = P.TRAFFIC_ENTRY.unpack_from(raw, off)
                off += P.TRAFFIC_ENTRY.size
                # 15 xpdrMode, 16 timeMs, 20 squawk, 21 xpdrIdent
                if e[1].rstrip(b"\x00") == b"ESXPA1" and e[16] == time_ms:
                    return [e[16], e[15], e[20], e[21]]   # time, mode, squawk, ident
        return []

    # An ordinary VFR aircraft, then an airliner squawking a discrete code.
    out["vfr"] = seen(a, 1200, P.XPDR_ALT, 0, 11)
    out["discrete"] = seen(a, 4671, P.XPDR_ALT, 0, 12)
    # Mode A, standby and off all have to arrive as themselves: the client
    # cannot make its own decision about TCAS if the server flattens them.
    out["mode_a"] = seen(a, 4671, P.XPDR_ON, 0, 13)
    out["standby"] = seen(a, 4671, P.XPDR_STANDBY, 0, 14)
    out["off"] = seen(a, 4671, P.XPDR_OFF, 0, 15)
    out["ident"] = seen(a, 4671, P.XPDR_ALT, 1, 16)
    out["emergency"] = seen(a, 7700, P.XPDR_ALT, 0, 17)
    out["highest_real_code"] = seen(a, 7777, P.XPDR_ALT, 0, 18)

    # Codes no transponder can produce: a squawk is four octal digits.
    out["digit_eight"] = seen(a, 7788, P.XPDR_ALT, 0, 19)
    out["digit_nine"] = seen(a, 1299, P.XPDR_ALT, 0, 20)
    out["over_7777"] = seen(a, 9999, P.XPDR_ALT, 0, 21)
    # And a mode outside the enum.
    out["mode_out_of_range"] = seen(a, 1200, 99, 0, 22)

    a.close(); b.close()
    return out


def scenario_crowded_sky(port):
    """More aircraft in range than fit in one packet.

    A traffic packet holds ten. With more than that about, the rest have to
    arrive in further packets -- not be dropped, and above all not be counted
    in a header that promises more entries than the packet carries, which a
    receiver reads as a position made of the next aircraft's bytes.
    """
    out = {}
    n = P.MAX_TRAFFIC_ENTRIES + 4          # enough to need two packets
    watcher = Client(port, "ESCRWD", 57.85, 27.02)
    watcher.login()
    others = []
    for i in range(n):
        c = Client(port, f"ESC{i:03d}", 57.85 + 0.001 * i, 27.02)
        c.login()
        others.append(c)

    for _ in range(3):
        for c in others:
            c.position(time_ms=100)
        watcher.position(time_ms=100)
        time.sleep(0.15)

    seen, packets, honest = set(), 0, True
    for raw in watcher.drain(1.0)[P.PT_TRAFFIC]:
        count, _ = P.TRAFFIC_HDR.unpack_from(raw, 0)
        packets += 1
        # The header must describe this packet, not the sender's intentions.
        if P.TRAFFIC_HDR.size + count * P.TRAFFIC_ENTRY.size > len(raw):
            honest = False
            continue
        if count > P.MAX_TRAFFIC_ENTRIES:
            honest = False
        off = P.TRAFFIC_HDR.size
        for _ in range(count):
            e = P.TRAFFIC_ENTRY.unpack_from(raw, off)
            off += P.TRAFFIC_ENTRY.size
            seen.add(e[1].rstrip(b"\x00").decode())

    out["others"] = n
    out["distinct_seen"] = len(seen)
    out["header_counts_are_honest"] = honest
    out["got_more_than_one_packet"] = packets > 1
    watcher.close()
    for c in others:
        c.close()
    return out


SCENARIOS = [
    ("login and traffic", scenario_login_and_traffic),
    ("login validation", scenario_bad_login),
    ("position validation", scenario_position_validation),
    ("text routing", scenario_text_routing),
    ("voice routing", scenario_voice_routing),
    ("session id enforcement", scenario_session_id),
    ("logout", scenario_logout),
    ("garbage packets", scenario_garbage),
    ("flight password", scenario_password),
    ("shared weather", scenario_weather),
    ("transponder", scenario_transponder),
    ("a crowded sky", scenario_crowded_sky),
]
# scenarios whose server runs with a flight password
PASSWORDED = {"flight password": "sky"}


# ---------------------------------------------------------------------------
# running a server
# ---------------------------------------------------------------------------
def wait_until_up(port, password="", timeout=8.0):
    probe = Client(port, "PROBE1", 0.0, 0.0)
    end = time.time() + timeout
    while time.time() < end:
        if probe.login(password=password) != 0:
            probe.send(P.PT_LOGOUT)
            probe.close()
            return True
        time.sleep(0.15)
    probe.close()
    return False


def run_against(label, make_cmd, base_port, cwd=None):
    """Run every scenario against its own fresh server.

    One long-lived server would let each scenario see the aircraft of the ones
    before it -- sessions live for 15 seconds -- and once more than 15 are
    logged in the traffic packet cap starts dropping the furthest, which makes
    the result depend on how fast the suite happened to run. A server per
    scenario costs a fraction of a second and removes the whole class of
    flake.
    """
    print(f"\n--- {label} ---")
    results = {}
    for i, (name, fn) in enumerate(SCENARIOS):
        port = base_port + i
        proc = subprocess.Popen(make_cmd(port, PASSWORDED.get(name, "")), cwd=cwd,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            if not wait_until_up(port, PASSWORDED.get(name, "")):
                print(f"  server did not come up on port {port}")
                return None
            results[name] = fn(port)
            print(f"  ran  {name}")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp", default=str(ROOT / "build-tests" / "xradio_server"))
    ap.add_argument("--port", type=int, default=49380)
    args = ap.parse_args()

    if not Path(args.cpp).exists():
        print(f"C++ server not built at {args.cpp} -- "
              f"configure with -DXRADIO_BUILD_TESTS=ON and build it first")
        return 1

    py = run_against(
        "python server",
        lambda p, pw: [sys.executable, str(ROOT / "server" / "server.py"), "--port", str(p)]
                      + (["--password", pw] if pw else []),
        args.port, cwd=str(ROOT / "server"))
    cpp = run_against(
        "built-in C++ server",
        lambda p, pw: [args.cpp, str(p)] + ([pw] if pw else []),
        args.port + len(SCENARIOS) + 1)

    if py is None or cpp is None:
        print("\none of the servers did not start")
        return 1

    print("\nthe two servers agree")
    for name, _ in SCENARIOS:
        same = py[name] == cpp[name]
        detail = ""
        if not same:
            for k in py[name]:
                if py[name][k] != cpp[name].get(k):
                    detail = f"{k}: python={py[name][k]!r} cpp={cpp[name].get(k)!r}"
                    break
        check(name, same, detail)

    # And the answers are the right ones, not just the same ones.
    print("\nand the answers are correct")
    r = cpp["login and traffic"]
    sids = [r["sid_a"], r["sid_b"], r["sid_far"]]
    check("sessions get consecutive ids",
          all(x > 0 for x in sids) and sids[1] == sids[0] + 1 and sids[2] == sids[1] + 1,
          str(sids))
    check("a nearby aircraft is in the traffic list",
          any(t[0] == "ESNB34" for t in r["a_sees"]))
    check("its ICAO type comes through",
          any(t[0] == "ESNB34" and t[1] == "A20N" for t in r["a_sees"]))
    check("an aircraft 130 nm away is not", not any(t[0] == "ESFAR1" for t in r["a_sees"]))

    r = cpp["shared weather"]
    check("the weather source's sky is relayed", r["relayed"])
    check("it arrives unchanged", r["intact"] == [43200.0, 180.0, 4.5, 99400.0, 7.0],
          str(r["intact"]))
    check("a pilot who did not claim it cannot set the weather", not r["stranger_relayed"])
    check("a truncated weather packet is dropped", not r["runt_relayed"])

    r = cpp["login validation"]
    check("an old protocol version is refused", r["old_protocol_gets_no_ack"])
    check("control characters are stripped from callsigns",
          all(all(ord(c) >= 32 for c in cs) for cs in r["callsigns_seen"]),
          str(r["callsigns_seen"]))
    check("an empty callsign gets a placeholder",
          any(cs.startswith("UNK") for cs in r["callsigns_seen"]), str(r["callsigns_seen"]))
    check("an empty aircraft type becomes ZZZZ", "ZZZZ" in r["ac_types_seen"],
          str(r["ac_types_seen"]))

    r = cpp["position validation"]
    states = [t for t in r["b_sees"] if t[0] == "ESVAL1"]
    check("every bad position is dropped, the last good one stands",
          len(states) == 1 and states[0][2] == 57.85 and states[0][8] == 1,
          str(states))

    r = cpp["text routing"]
    check("text reaches someone on the same frequency",
          any(t[2] == "hello all" for t in r["same_freq"]), str(r["same_freq"]))
    check("text does not reach another frequency", r["other_freq"] == [])
    check("text does not reach past the VHF horizon", r["out_of_range"] == [])
    check("a transmission on an untuned radio is refused",
          not any(t[2] == "wrong radio" for t in r["same_freq"]))
    long_ones = [t for t in r["same_freq"] if t[2].startswith("xxx")]
    check("over-long text is clamped to 200 bytes",
          len(long_ones) == 1 and len(long_ones[0][2]) == 200,
          str(len(long_ones[0][2]) if long_ones else "none"))

    r = cpp["voice routing"]
    check("voice reaches the same frequency", len(r["heard"]) == 5, str(len(r["heard"])))
    check("voice payload survives intact",
          all(v[2] == b"\x01\x02\x03\x04" for v in r["heard"]))
    check("voice does not reach another frequency", r["not_heard"] == [])
    check("a talking aircraft is flagged as transmitting",
          ("ESVCA1", 1) in r["tx_flags"], str(r["tx_flags"]))

    r = cpp["session id enforcement"]
    check("a position with the wrong session id is ignored",
          all(t[2] != 10.0 for t in r["b_sees"]), str(r["b_sees"]))
    check("text with the wrong session id is ignored", r["b_texts"] == [])
    check("ping still answers with the right id", r["pong"])

    r = cpp["logout"]
    check("logout removes the aircraft",
          "ESOUT1" in r["before"] and "ESOUT1" not in r["after"],
          f"{r['before']} -> {r['after']}")

    r = cpp["garbage packets"]
    check("a working session survives a burst of junk",
          any(t[0] == "ESGAR1" for t in r["still_working"]), str(r["still_working"]))

    r = cpp["flight password"]
    check("no password is refused and told why",
          r["without_password_sid"] == 0 and r["without_password_reason"] == P.RJ_PASSWORD)
    check("a wrong password is refused and told why",
          r["wrong_password_sid"] == 0 and r["wrong_password_reason"] == P.RJ_PASSWORD)
    check("the right password gets in", r["right_password_joined"])
    check("an old version is told it is an old version",
          r["old_version_reason"] == P.RJ_VERSION and r["old_header_reason"] == P.RJ_VERSION,
          str((r["old_version_reason"], r["old_header_reason"])))
    check("the livery given at login is relayed in traffic", r["livery_seen"] == ["Lufthansa"],
          str(r["livery_seen"]))

    r = cpp["transponder"]
    # [timeMs, mode, squawk, ident]
    check("an ordinary VFR squawk is relayed untouched",
          r["vfr"][1:] == [P.XPDR_ALT, 1200, 0], str(r["vfr"]))
    check("so is a discrete code", r["discrete"][1:] == [P.XPDR_ALT, 4671, 0],
          str(r["discrete"]))
    check("mode A arrives as mode A, not flattened to mode C",
          r["mode_a"][1:] == [P.XPDR_ON, 4671, 0], str(r["mode_a"]))
    check("standby arrives as standby, so the client can drop it from TCAS",
          r["standby"][1:] == [P.XPDR_STANDBY, 4671, 0], str(r["standby"]))
    check("and off as off", r["off"][1:] == [P.XPDR_OFF, 4671, 0], str(r["off"]))
    check("ident is relayed", r["ident"][1:] == [P.XPDR_ALT, 4671, 1], str(r["ident"]))
    check("an emergency squawk is passed on, not swallowed",
          r["emergency"][1:] == [P.XPDR_ALT, 7700, 0], str(r["emergency"]))
    check("7777 is a real code and survives",
          r["highest_real_code"][1:] == [P.XPDR_ALT, 7777, 0],
          str(r["highest_real_code"]))
    # Octal: no digit above 7, nothing above 7777.
    check("a squawk with an 8 in it is not relayed as real",
          r["digit_eight"][2] == 0, str(r["digit_eight"]))
    check("nor one with a 9", r["digit_nine"][2] == 0, str(r["digit_nine"]))
    check("nor one above 7777", r["over_7777"][2] == 0, str(r["over_7777"]))
    check("a mode outside the enum becomes off, not something random",
          r["mode_out_of_range"][1] == P.XPDR_OFF, str(r["mode_out_of_range"]))

    r = cpp["a crowded sky"]
    check("a header never promises more entries than its packet carries",
          r["header_counts_are_honest"])
    check("more aircraft than fit in one packet are sent in several",
          r["got_more_than_one_packet"])
    check("and none of them is dropped on the way",
          r["distinct_seen"] == r["others"],
          f"{r['distinct_seen']} of {r['others']}")

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("both servers behave identically, and correctly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
