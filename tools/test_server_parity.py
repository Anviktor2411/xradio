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
    """`agl_ft`: the height above the ground it reports (v6); left out, the
    country is flat at sea level and it is the altitude -- a station passes 0
    for its facility's antenna. `proto=5` makes it a v5 client: v5 headers and
    the 72-byte position without the radio fields."""

    def __init__(self, port, callsign, lat, lon, alt_ft=3000.0, com1=FREQ, com2=0,
                 ac="C172", agl_ft=None, on_ground=False, proto=P.PROTO_VERSION):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.25)
        self.dest = ("127.0.0.1", port)
        self.callsign = callsign
        self.ac = ac
        self.lat, self.lon, self.alt_ft = lat, lon, alt_ft
        self.agl_ft = alt_ft if agl_ft is None else agl_ft
        self.on_ground = on_ground
        self.com1, self.com2 = com1, com2
        self.proto = proto
        self.sid = 0

    def send(self, ptype, payload=b"", sid=None):
        self.sock.sendto(P.pack(ptype, self.sid if sid is None else sid, payload, self.proto),
                         self.dest)

    def login(self, proto=None, callsign=None, ac=None, livery="",
              password="", flags=0):
        self.send(P.PT_LOGIN, P.LOGIN.pack(
            P.pad(callsign if callsign is not None else self.callsign, 16),
            P.pad(ac if ac is not None else self.ac, 8), self.proto if proto is None else proto, flags,
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
                 squawk=1200, xpdr=P.XPDR_ALT, ident=0, agl_ft=None, svc_range=0, svc_ceiling=0):
        fields = (self.lat if lat is None else lat,
                  self.lon if lon is None else lon,
                  (self.alt_ft if alt_ft is None else alt_ft) / 3.28084,
                  heading, 0.0, 0.0, 0.0 if self.on_ground else gs, gear, flap,
                  self.com1, self.com2, 0, 1 if self.on_ground else 0, tx, rx, time_ms, track, vs,
                  squawk, xpdr, ident)
        if self.proto >= 6:
            agl = (self.agl_ft if agl_ft is None else agl_ft) / 3.28084
            self.send(P.PT_POSITION, P.POSITION.pack(*fields, agl, svc_range, svc_ceiling), sid=sid)
        else:
            self.send(P.PT_POSITION, P.POSITION_V5.pack(*fields), sid=sid)

    def text(self, body, freq=0, sid=None, to=""):
        raw = body.encode()
        self.send(P.PT_TEXT,
                  P.TEXT_HDR.pack(freq, 0, P.pad(self.callsign, 16), len(raw),
                                  P.pad(to, 16)) + raw,
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
            except ConnectionResetError:
                # Windows only: a datagram sent earlier was refused -- nothing
                # was listening yet, which is exactly what the probe that
                # waits for a server to come up sends into -- and the refusal
                # is reported on the next read. Not an answer; keep waiting.
                continue
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
            except ConnectionResetError:
                continue                # see recv()
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
            # And the height above the ground the radio is routed by.
            seen.add((cs, P.cstr(e[2]),
                      round(e[3], 4), round(e[4], 4), round(e[5], 1),
                      round(e[6], 1), round(e[9], 1), round(e[18], 1),
                      e[16], e[12], e[13], round(e[23], 1)))
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
        freq, _sid, frm, n, _to = P.TEXT_HDR.unpack_from(payload, 0)
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


def scenario_guard(port):
    """121.500 reaches everyone in range, whatever they have tuned.

    It is the one frequency you can call somebody on without already knowing
    where they are listening. Two things must stay true anyway: it is a radio,
    so the horizon still applies, and you still cannot transmit on a frequency
    your own radio is not tuned to.
    """
    out = {}
    caller = Client(port, "ESGDA1", 57.85, 27.02, com1=P.GUARD_KHZ)
    near = Client(port, "ESGDB1", 57.855, 27.025, com1=OTHER_FREQ)
    near2 = Client(port, "ESGDC1", 57.856, 27.026, com1=118500)
    far = Client(port, "ESGDD1", 60.50, 27.02, com1=OTHER_FREQ)
    for c in (caller, near, near2, far):
        c.login()
        c.position()
    time.sleep(0.2)

    caller.text("anyone on guard, come to 118.100", freq=P.GUARD_KHZ)
    time.sleep(0.3)
    out["heard_by_other_freq"] = texts(near.drain(0.3)[P.PT_TEXT])
    out["heard_by_another"] = texts(near2.drain(0.2)[P.PT_TEXT])
    out["past_the_horizon"] = texts(far.drain(0.2)[P.PT_TEXT])

    # Voice on guard, the same way.
    for i in range(5):
        caller.voice(seq=i, freq=P.GUARD_KHZ)
        time.sleep(0.02)
    time.sleep(0.25)
    out["voice_heard"] = len(near.drain(0.3)[P.PT_VOICE])
    out["voice_past_the_horizon"] = len(far.drain(0.2)[P.PT_VOICE])

    # A pilot who has not tuned guard still cannot transmit on it.
    near.text("not tuned there", freq=P.GUARD_KHZ)
    time.sleep(0.25)
    out["untuned_sender_relayed"] = texts(near2.drain(0.3)[P.PT_TEXT])

    for c in (caller, near, near2, far):
        c.close()
    return out


def scenario_direct_message(port):
    """A message addressed to a callsign is not a transmission.

    It reaches that one pilot wherever they are and whatever they have tuned,
    it reaches nobody else, and a callsign that is not in the flight comes
    back as an answer rather than silence.
    """
    out = {}
    a = Client(port, "ESDMA1", 57.85, 27.02)
    b = Client(port, "ESDMB1", 60.50, 27.02, com1=OTHER_FREQ)   # far away, other freq
    nosy = Client(port, "ESDMC1", 57.851, 27.021)               # same freq, right there
    for c in (a, b, nosy):
        c.login()
        c.position()
    time.sleep(0.2)

    a.text("come to 118.100", to="ESDMB1")
    time.sleep(0.3)
    out["reached_the_target"] = texts(b.drain(0.4)[P.PT_TEXT])
    out["nobody_else_saw_it"] = texts(nosy.drain(0.2)[P.PT_TEXT])

    # An unknown callsign is answered, not swallowed.
    a.text("anyone there", to="NOSUCH")
    time.sleep(0.3)
    out["unknown_callsign_answer"] = texts(a.drain(0.4)[P.PT_TEXT])

    # And the target's own reply goes back the same way.
    b.text("on my way", to="ESDMA1")
    time.sleep(0.3)
    out["reply_came_back"] = texts(a.drain(0.4)[P.PT_TEXT])

    for c in (a, b, nosy):
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
    b.drain(0.3)        # what was sent before the logout took effect is not the answer
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

    # The source logs in again from the same address -- a lost login ack, or
    # a client that logs in again after a network stall -- and must still be
    # the source: the old session's claim has to go with the old session.
    src.send(P.PT_LOGIN, P.LOGIN.pack(P.pad("ESWXA1", 16), P.pad("C172", 8),
             P.PROTO_VERSION, P.LF_WEATHER_SOURCE, P.pad("", 16), P.pad("", 32)), sid=0)
    ack = src.recv(P.PT_LOGIN_ACK)
    new_sid = P.LOGIN_ACK.unpack_from(ack, 0)[0] if ack else 0
    out["relogin_got_new_session"] = new_sid != 0 and new_sid != src.sid
    src.sid = new_sid
    src.send(P.PT_WEATHER, P.WEATHER.pack(*sky))
    time.sleep(0.2)
    out["relayed_after_relogin"] = len(other.drain(0.4)[P.PT_WEATHER]) > 0

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


def roster_of(packets, keep=None):
    """The callsigns in the newest complete roster among `packets`.

    Returns (names, honest, parts): `honest` is whether every packet's header
    described exactly what that packet carried, and `parts` how many packets
    the newest complete roster came in. A roster split over several packets
    only counts once every part of one epoch has arrived."""
    epochs, order, honest = {}, [], True
    for raw in packets:
        if len(raw) < P.ROSTER_HDR.size:
            honest = False
            continue
        epoch, part, parts, count, _ = P.ROSTER_HDR.unpack_from(raw, 0)
        if (P.ROSTER_HDR.size + count * P.ROSTER_ENTRY.size != len(raw)
                or part >= parts or count > P.MAX_ROSTER_ENTRIES):
            honest = False
            continue
        names, off = [], P.ROSTER_HDR.size
        for _ in range(count):
            _sid, cs, ac = P.ROSTER_ENTRY.unpack_from(raw, off)
            off += P.ROSTER_ENTRY.size
            names.append((norm(P.cstr(cs)), P.cstr(ac)))
        e = epochs.setdefault(epoch, {"parts": parts, "got": {}})
        e["got"][part] = names
        if len(e["got"]) == e["parts"]:
            order.append(epoch)
    if not order:
        return None, honest, 0
    newest = epochs[order[-1]]
    names = sorted(n for part in newest["got"].values() for n in part
                   if keep is None or n[0].startswith(tuple(keep)))
    return names, honest, newest["parts"]


def scenario_roster(port):
    """Everyone in the flight reaches everyone, for the @ list.

    Traffic stops at 80 nm, but a message to @CALLSIGN reaches a pilot
    anywhere in the flight, so the roster has to include the pilot nobody can
    see. It follows logins and logouts within a tick, and comes again by
    itself so one lost packet does not leave a stale list."""
    out = {}
    a = Client(port, "RSTA01", 57.85, 27.02, ac="C172")
    b = Client(port, "RSTB02", 57.86, 27.03, ac="B738")
    far = Client(port, "RSTF03", 59.94, 30.31, ac="A20N")     # ~190 nm away
    a.login(); b.login(); far.login()
    for c in (a, b, far):
        c.position()
    time.sleep(0.4)
    names, honest, _ = roster_of(a.drain(0.4)[P.PT_ROSTER], ("RST",))
    out["everyone"] = names
    out["honest"] = honest

    b.send(P.PT_LOGOUT)
    time.sleep(0.35)
    names, honest, _ = roster_of(a.drain(0.4)[P.PT_ROSTER], ("RST",))
    out["after_logout"] = names
    out["honest_after_logout"] = honest

    # Nobody joins or leaves: the roster still comes, on its own clock.
    a.drain(0.2)
    far.position()
    a.position()
    waited = far.drain(5.6)[P.PT_ROSTER]
    names, _, _ = roster_of(waited, ("RST",))
    out["refreshed_unprompted"] = names
    a.close(); b.close(); far.close()
    return out


def scenario_big_roster(port):
    """More pilots than fit in one roster packet arrive in parts."""
    out = {}
    n = P.MAX_ROSTER_ENTRIES + 4
    watcher = Client(port, "RSBWAT", 57.85, 27.02)
    watcher.login()
    others = []
    for i in range(n):
        c = Client(port, f"RB{i:03d}", 57.85, 27.02)
        c.login()
        others.append(c)
    time.sleep(0.4)
    names, honest, parts = roster_of(watcher.drain(0.6)[P.PT_ROSTER], ("RB", "RSB"))
    out["count"] = len(names or [])
    out["honest"] = honest
    out["parts"] = parts
    watcher.close()
    for c in others:
        c.close()
    return out


def atc_list_of(packets, with_flags=False):
    """The newest ATC list in a burst, as (callsign, icao, freq, kind) rows
    followed by where the station transmits from and what it covers. With
    `with_flags`, (header flags, rows)."""
    rows, flags = [], None
    for payload in packets:
        if len(payload) < P.ATC_LIST_HDR.size:
            continue
        count, hflags = P.ATC_LIST_HDR.unpack_from(payload, 0)
        off, cur = P.ATC_LIST_HDR.size, []
        for _ in range(count):
            if off + P.ATC_ENTRY.size > len(payload):
                break
            e = P.ATC_ENTRY.unpack_from(payload, off)
            off += P.ATC_ENTRY.size
            cur.append((P.cstr(e[1]), P.cstr(e[2]), e[3], e[4],
                        e[5], round(e[7], 4), round(e[8], 4), round(e[9], 1), round(e[10], 1), e[11], e[12]))
        rows, flags = cur, hflags
    return (flags, rows) if with_flags else rows


def scenario_atc_v6(port):
    """v6: a station, an injector and its aircraft, flight plans, the ATIS."""
    twr = Client(port, "EETN_TWR", 57.85, 27.02, alt_ft=150.0, ac="ATC", agl_ft=0.0)
    inj = Client(port, "TTRAIN_AI", 57.85, 27.02, ac="ZZZZ", com1=0)
    a = Client(port, "ESNA12", 57.85, 27.02)
    b = Client(port, "ESNB34", 57.86, 27.03, ac="A20N")
    out = {}
    out["sid_twr"] = twr.login(flags=P.LF_ATC)
    out["sid_inj"] = inj.login(flags=P.LF_INJECTOR)
    out["sid_a"] = a.login()
    out["sid_b"] = b.login()
    mine = ("ESNA12", "ESNB34", "EETN_TWR", "TTRAIN_AI", "FIN", "BTI", "GHOST")

    def ai_entry(local, cs, lat, lon, flags=0):
        return P.TRAFFIC_ENTRY.pack(local, P.pad(cs, 16), P.pad("A320", 8), lat, lon, 500.0, 90.0, 0.0, 0.0,
                                    70.0, 1.0, 0.0, 0, 0, 0, P.XPDR_ALT, 4242, 90.0, 0.0, P.pad("", 16),
                                    4521, 0, flags, 480.0)

    def inject(entries):
        inj.send(P.PT_AI_TRAFFIC, P.TRAFFIC_HDR.pack(len(entries), 0) + b"".join(entries))

    bad = P.TRAFFIC_ENTRY.pack(7, P.pad("GHOST", 16), P.pad("A320", 8), 500.0, 27.0, 500.0, 90.0, 0.0, 0.0,
                               70.0, 1.0, 0.0, 0, 0, 0, P.XPDR_ALT, 0, 90.0, 0.0, P.pad("", 16), 4521, 0, 0,
                               480.0)
    for _ in range(4):
        a.position(); b.position(); twr.position(); inj.position()
        inject([ai_entry(1, "FIN321", 57.86, 27.03), ai_entry(2, "BTI101", 57.87, 27.04), bad])
        time.sleep(0.12)
    out["a_sees"] = traffic_set(wait_traffic(a, want="FIN321"), mine)
    out["twr_sees"] = traffic_set(wait_traffic(twr, want="FIN321"), mine)
    out["inj_sees"] = traffic_set(inj.drain(0.4)[P.PT_TRAFFIC], mine)
    out["atc_list"] = atc_list_of(a.drain(0.3).get(P.PT_ATC_LIST, []))

    # the injector speaks as its aircraft, and as a ghost it does not drive
    aisid = P.ai_sid(inj.sid, 1)
    msg = b"Finnair 321, established"
    inj.send(P.PT_TEXT, P.TEXT_HDR.pack(FREQ, aisid, P.pad("FIN321", 16), len(msg), P.pad("", 16)) + msg)
    inj.send(P.PT_TEXT, P.TEXT_HDR.pack(FREQ, P.ai_sid(inj.sid, 9), P.pad("GHOST", 16), 5, P.pad("", 16)) + b"ghost")
    inj.send(P.PT_VOICE, P.VOICE_HDR.pack(FREQ, aisid, 3, 4) + b"\x01\x02\x03\x04")
    time.sleep(0.3)
    got_b = b.drain(0.4)
    out["b_texts"] = [(f, n, body, sid == aisid) for (f, n, body), sid in
                      zip(texts(got_b[P.PT_TEXT]), [P.TEXT_HDR.unpack_from(p, 0)[1] for p in sorted(got_b[P.PT_TEXT])])]
    out["b_voices"] = voices(got_b[P.PT_VOICE])
    out["twr_texts"] = texts(twr.drain(0.3)[P.PT_TEXT])
    out["inj_texts"] = texts(inj.drain(0.3)[P.PT_TEXT])

    # flight plans: filed, acknowledged, cancelled; a late station
    a.send(P.PT_FLIGHTPLAN, P.flightplan("SPOOF", "C172", "EETN", "EFHK", "DCT", rules="V", cruise_ft=3500, squawk=7777))
    inj.send(P.PT_FLIGHTPLAN, P.flightplan("TTRAIN_AI", "ZZZZ", "EETN", "EFHK"))
    time.sleep(0.3)
    fp_twr = [P.unpack_flightplan(p) for p in twr.drain(0.3).get(P.PT_FLIGHTPLAN, [])]
    out["fp_at_station"] = sorted((d["callsign"], d["session_id"] == a.sid, d["status"], d["squawk"], d["dest"], d["route"]) for d in fp_twr)
    twr.send(P.PT_FLIGHTPLAN, P.flightplan("ESNA12", "C172", "EETN", "EFHK", "DCT", status=P.FP_ACK, squawk=4521,
                                           session_id=a.sid, remarks="cleared as filed"))
    time.sleep(0.3)
    fp_a = [P.unpack_flightplan(p) for p in a.drain(0.3).get(P.PT_FLIGHTPLAN, [])]
    out["fp_ack"] = sorted((d["status"], d["squawk"], d["remarks"]) for d in fp_a)
    out["fp_b"] = len(b.drain(0.2).get(P.PT_FLIGHTPLAN, []))
    gnd = Client(port, "EETN_GND", 57.85, 27.02, alt_ft=150.0, ac="ATC", com1=OTHER_FREQ, agl_ft=0.0)
    out["sid_gnd"] = gnd.login(flags=P.LF_ATC)
    late = [P.unpack_flightplan(p) for p in gnd.drain(0.4).get(P.PT_FLIGHTPLAN, [])]
    out["fp_late"] = sorted((d["callsign"], d["squawk"], d["status"]) for d in late)

    # the ATIS: offered on tuning, once per letter, again on a new letter
    twr.send(P.PT_ATIS, P.atis(127905, "EETN", "C", "Tallinn information Charlie"))
    a.send(P.PT_ATIS, P.atis(127905, "EETN", "Z", "not a station"))
    time.sleep(0.3)
    b.com2 = 127905
    b.position(rx=P.RX_COM1 | P.RX_COM2)
    b.position(rx=P.RX_COM1 | P.RX_COM2)
    time.sleep(0.3)
    got = b.drain(0.3)
    out["atis_b"] = sorted((P.ATIS_HDR.unpack_from(p, 0)[2], p[P.ATIS_HDR.size:]) for p in got.get(P.PT_ATIS, []))
    out["atis_a"] = len(a.drain(0.2).get(P.PT_ATIS, []))
    twr.send(P.PT_ATIS, P.atis(127905, "EETN", "D", "Tallinn information Delta"))
    time.sleep(0.3)
    out["atis_b2"] = sorted(P.ATIS_HDR.unpack_from(p, 0)[2] for p in b.drain(0.3).get(P.PT_ATIS, []))
    out["atc_list2"] = atc_list_of(a.drain(0.5).get(P.PT_ATC_LIST, []))

    # removal, and what leaves with the injector. Packets already queued at
    # the client from before the change are drained away first: the sample
    # has to be what the server sends after, not what was in flight.
    inject([ai_entry(1, "FIN321", 57.86, 27.03), ai_entry(2, "BTI101", 57.87, 27.04, flags=P.AI_REMOVE)])
    a.drain(0.5)
    out["after_remove"] = traffic_set(a.drain(0.4)[P.PT_TRAFFIC], ("FIN", "BTI"))
    inj.send(P.PT_LOGOUT)
    a.drain(0.5)
    out["after_injector_left"] = traffic_set(a.drain(0.4)[P.PT_TRAFFIC], ("FIN", "BTI"))
    twr.send(P.PT_LOGOUT)
    a.drain(0.3)
    out["atc_list3"] = atc_list_of(a.drain(0.5).get(P.PT_ATC_LIST, []))
    for c in (twr, inj, a, b, gnd):
        c.close()
    return out


# ---------------------------------------------------------------------------
# the radio model (docs/radio.md)
# ---------------------------------------------------------------------------
LOWI = (47.2602, 11.3439)     # Innsbruck: an apron at 1900 ft, in a valley


def west_of_lowi(nm):
    return LOWI[1] - nm / (60.0 * 0.678575)


def scenario_radio_heights(port):
    """Heights above the ground, a tower's service volume, FL350 to a ground
    station -- both servers must route every one of these the same way."""
    APRON, TOWER = 121900, 120100
    out = {}
    ap1 = Client(port, "OELWA1", LOWI[0], LOWI[1], alt_ft=1906.0, agl_ft=0.0, on_ground=True, com1=APRON)
    ap2 = Client(port, "OELWA2", LOWI[0] + 0.04, LOWI[1], alt_ft=1906.0, agl_ft=0.0, on_ground=True, com1=APRON)
    ap3 = Client(port, "OELWA3", LOWI[0] + 0.1667, LOWI[1], alt_ft=1906.0, agl_ft=0.0, on_ground=True, com1=APRON)
    twr = Client(port, "LOWI_TWR", LOWI[0], LOWI[1], alt_ft=2006.0, agl_ft=0.0, on_ground=True, com1=TOWER,
                 ac="ATC")
    fin = Client(port, "OEFIN1", LOWI[0], west_of_lowi(10.0), alt_ft=4900.0, agl_ft=3000.0, com1=TOWER)
    rim = Client(port, "OERIM1", LOWI[0], west_of_lowi(20.0), alt_ft=1950.0, agl_ft=0.0, on_ground=True,
                 com1=TOWER)
    hold = Client(port, "OEHLD1", LOWI[0], LOWI[1] + 0.01, alt_ft=1906.0, agl_ft=0.0, on_ground=True, com1=TOWER)
    hi = Client(port, "DLH4AB", LOWI[0] + 3.5, LOWI[1], alt_ft=35000.0, agl_ft=34000.0, com1=TOWER)
    hi2 = Client(port, "DLH4AC", LOWI[0] + 4.4, LOWI[1], alt_ft=35000.0, agl_ft=34000.0, com1=TOWER)
    everyone = (ap1, ap2, ap3, twr, fin, rim, hold, hi, hi2)
    for c in everyone:
        c.login(flags=P.LF_ATC if c is twr else 0)
    for _ in range(2):
        for c in everyone:
            c.position()
        time.sleep(0.12)
    ap1.text("Innsbruck Ground, OELWA1, radio check", freq=APRON)
    twr.text("Innsbruck Tower, all stations", freq=TOWER)
    time.sleep(0.3)
    got = {c.callsign: texts(c.drain(0.3)[P.PT_TEXT]) for c in everyone}
    out["apron_2nm"] = got["OELWA2"]
    out["apron_10nm"] = got["OELWA3"]
    out["tower_to_final"] = got["OEFIN1"]
    out["tower_to_20nm_on_ground"] = got["OERIM1"]
    out["tower_to_fl350_210nm"] = got["DLH4AB"]
    out["tower_to_fl350_264nm"] = got["DLH4AC"]
    rim.text("OERIM1, request taxi", freq=TOWER)
    hi.text("DLH4AB, FL350", freq=TOWER)
    time.sleep(0.3)
    out["tower_hears"] = texts(twr.drain(0.3)[P.PT_TEXT])
    out["pilot_at_airport_hears"] = texts(hold.drain(0.3)[P.PT_TEXT])
    # and the height above the ground each one is routed by, as the traffic carries it
    out["traffic_agl"] = [(t[0], t[-1]) for t in
                          traffic_set(wait_traffic(ap1, want="OEFIN1"), ("OELWA", "OEFIN", "OERIM"))]
    for c in everyone:
        c.close()
    return out


def raw_packets(c, seconds):
    """Everything that arrives as (type, header version, payload)."""
    out = []
    end = time.time() + seconds
    while time.time() < end:
        c.sock.settimeout(max(0.01, end - time.time()))
        try:
            data, _ = c.sock.recvfrom(2048)
        except socket.timeout:
            break
        except ConnectionResetError:
            continue
        parsed = P.unpack_header(data)
        if parsed is not None:
            out.append((parsed[0], parsed[1], parsed[4]))
    return out


def scenario_v5_client(port):
    """A v5 pilot (XRadio 0.5) in a v6 flight: let in, sent what v5 knows in
    the shape v5 reads it, routed by its altitude in the air and as 0 above
    the ground on the ground."""
    APRON = 121900
    out = {}
    new = Client(port, "OELWA1", LOWI[0], LOWI[1], alt_ft=1906.0, agl_ft=0.0, on_ground=True, com1=APRON)
    old = Client(port, "OELWB1", LOWI[0] + 0.01, LOWI[1], alt_ft=1906.0, on_ground=True, com1=APRON, proto=5)
    far5 = Client(port, "OELWB2", LOWI[0] + 0.1667, LOWI[1], alt_ft=1906.0, on_ground=True, com1=APRON, proto=5)
    up5 = Client(port, "OELWB3", LOWI[0] + 1.9, LOWI[1], alt_ft=9000.0, com1=APRON, proto=5)   # 114 nm, airborne
    out["logged_in"] = [c.login() != 0 for c in (new, old, far5, up5)]
    for _ in range(3):
        for c in (new, old, far5, up5):
            c.position()
        time.sleep(0.12)
    got = raw_packets(old, 1.0)
    out["versions"] = sorted({v for _t, v, _p in got})
    out["kinds"] = sorted({t for t, _v, _p in got})
    sizes_ok, names = True, set()
    for t, _v, payload in got:
        if t != P.PT_TRAFFIC:
            continue
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        sizes_ok &= len(payload) == P.TRAFFIC_HDR.size + count * P.TRAFFIC_ENTRY_V5.size
        for k in range(count):
            if P.TRAFFIC_HDR.size + (k + 1) * P.TRAFFIC_ENTRY_V5.size <= len(payload):
                e = P.TRAFFIC_ENTRY_V5.unpack_from(payload, P.TRAFFIC_HDR.size + k * P.TRAFFIC_ENTRY_V5.size)
                names.add(P.cstr(e[1]))
    out["v5_sized_entries"] = sizes_ok
    out["sees"] = sorted(n for n in names if n.startswith("OELW"))
    new.text("OELWA1, taxiing to 26", freq=APRON)
    time.sleep(0.3)
    out["old_hears_new"] = texts(old.drain(0.3)[P.PT_TEXT])
    out["far5_hears_new"] = texts(far5.drain(0.3)[P.PT_TEXT])
    out["up5_hears_new"] = texts(up5.drain(0.3)[P.PT_TEXT])
    old.text("OELWB1, roger", freq=APRON)
    time.sleep(0.3)
    out["new_hears_old"] = texts(new.drain(0.3)[P.PT_TEXT])
    # a v4 client is still told no -- in a header it can read
    v4 = Client(port, "OELWB4", LOWI[0], LOWI[1], proto=4)
    v4.send(P.PT_LOGIN, P.login("OELWB4", "C172", version=4), sid=0)
    rej = [(v, P.LOGIN_REJECT.unpack_from(p, 0)[0]) for t, v, p in raw_packets(v4, 0.6) if t == P.PT_LOGIN_REJECT]
    out["v4_rejected"] = rej
    for c in (new, old, far5, up5, v4):
        c.close()
    return out


def warm_still_weather(temp_c=24.0):
    """Warm, high pressure, calm and dry, at an hour that ducts (19500 days, 07:00)."""
    w = [0, 7 * 3600.0, 19500, 10.0, 102200.0, temp_c, 0.0, 0.0, 0.0, 0] + [0.0] * (P.WEATHER.size // 4 - 10)
    w[35] = 2.5
    return P.WEATHER.pack(*w)


def ducting_run(port):
    SEA = 123450
    out = {}
    sea1 = Client(port, "ESSEA1", 59.60, 22.00, alt_ft=500.0, com1=SEA)
    sea2 = Client(port, "ESSEA2", 59.60, 24.635, alt_ft=500.0, com1=SEA)     # 80 nm east, both at 500 ft
    wx = Client(port, "ESWXD1", 59.60, 23.30, alt_ft=500.0, com1=SEA)
    sea1.login(); sea2.login()
    wx.login(flags=P.LF_WEATHER_SOURCE)
    for c in (sea1, sea2, wx):
        c.position()
    time.sleep(0.2)
    sea1.text("over the Gulf, before", freq=SEA)
    time.sleep(0.3)
    out["before"] = texts(sea2.drain(0.3)[P.PT_TEXT])
    wx.send(P.PT_WEATHER, warm_still_weather())
    time.sleep(0.4)
    out["flags_warm"] = atc_list_of(sea2.drain(0.4).get(P.PT_ATC_LIST, []), with_flags=True)[0]
    for c in (sea1, sea2, wx):
        c.position()
    time.sleep(0.2)
    sea1.text("over the Gulf, in the duct", freq=SEA)
    time.sleep(0.3)
    out["warm"] = texts(sea2.drain(0.3)[P.PT_TEXT])
    wx.send(P.PT_WEATHER, warm_still_weather(8.0))
    time.sleep(0.4)
    out["flags_cold"] = atc_list_of(sea2.drain(0.4).get(P.PT_ATC_LIST, []), with_flags=True)[0]
    sea1.text("over the Gulf, after", freq=SEA)
    time.sleep(0.3)
    out["cold"] = texts(sea2.drain(0.3)[P.PT_TEXT])
    for c in (sea1, sea2, wx):
        c.close()
    return out


def scenario_ducting(port):
    """The realism switch on (--ducting): warm, still weather carries VHF far over the sea."""
    return ducting_run(port)


def scenario_ducting_off(port):
    """...and off, as every server starts: the same weather changes nothing."""
    return ducting_run(port)


SCENARIOS = [
    ("login and traffic", scenario_login_and_traffic),
    ("atc v6", scenario_atc_v6),
    ("radio heights", scenario_radio_heights),
    ("v5 client", scenario_v5_client),
    ("ducting", scenario_ducting),
    ("ducting off", scenario_ducting_off),
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
    ("guard", scenario_guard),
    ("direct messages", scenario_direct_message),
    ("roster", scenario_roster),
    ("a big roster", scenario_big_roster),
]
# scenarios whose server runs with a flight password
PASSWORDED = {"flight password": "sky"}
# ...and with other switches, the same on both servers
EXTRA_ARGS = {"ducting": ["--ducting"]}


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
        proc = subprocess.Popen(make_cmd(port, PASSWORDED.get(name, "")) + EXTRA_ARGS.get(name, []), cwd=cwd,
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

    r = cpp["roster"]
    check("the roster lists everyone in the flight",
          [n for n, _ in (r["everyone"] or [])] == ["RSTA01", "RSTB02", "RSTF03"],
          str(r["everyone"]))
    check("including a pilot far outside traffic range",
          any(n == "RSTF03" for n, _ in (r["everyone"] or [])))
    check("with their aircraft type", ("RSTB02", "B738") in (r["everyone"] or []))
    check("every roster header tells the truth", r["honest"] and r["honest_after_logout"])
    check("a pilot who logs out leaves it",
          [n for n, _ in (r["after_logout"] or [])] == ["RSTA01", "RSTF03"],
          str(r["after_logout"]))
    check("it comes again unprompted",
          [n for n, _ in (r["refreshed_unprompted"] or [])] == ["RSTA01", "RSTF03"],
          str(r["refreshed_unprompted"]))
    r = cpp["a big roster"]
    check("a roster too big for one packet comes in two", r["parts"] == 2, str(r["parts"]))
    check("and loses nobody", r["count"] == P.MAX_ROSTER_ENTRIES + 5, str(r["count"]))
    check("with honest headers", r["honest"])

    r = cpp["shared weather"]
    check("the weather source's sky is relayed", r["relayed"])
    check("it arrives unchanged", r["intact"] == [43200.0, 180.0, 4.5, 99400.0, 7.0],
          str(r["intact"]))
    check("a pilot who did not claim it cannot set the weather", not r["stranger_relayed"])
    check("a truncated weather packet is dropped", not r["runt_relayed"])
    check("logging in again gives the source a new session", r["relogin_got_new_session"])
    check("and its sky is still relayed afterwards: the claim moved with it",
          r["relayed_after_relogin"])

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

    r = cpp["guard"]
    guard_call = [(P.GUARD_KHZ, "ESGDA1", "anyone on guard, come to 118.100")]
    check("guard reaches a pilot on another frequency",
          r["heard_by_other_freq"] == guard_call, str(r["heard_by_other_freq"]))
    check("and everyone else in range too",
          r["heard_by_another"] == guard_call, str(r["heard_by_another"]))
    check("relayed on guard, not rewritten to the listener's frequency",
          all(t[0] == P.GUARD_KHZ for t in r["heard_by_other_freq"]),
          str(r["heard_by_other_freq"]))
    check("but not past the VHF horizon -- it is still a radio",
          r["past_the_horizon"] == [], str(r["past_the_horizon"]))
    check("voice on guard is heard the same way", r["voice_heard"] == 5,
          str(r["voice_heard"]))
    check("and stops at the horizon the same way",
          r["voice_past_the_horizon"] == 0, str(r["voice_past_the_horizon"]))
    check("a pilot who has not tuned guard still cannot transmit on it",
          r["untuned_sender_relayed"] == [], str(r["untuned_sender_relayed"]))

    r = cpp["direct messages"]
    check("a direct message reaches the pilot it names, however far away",
          r["reached_the_target"] == [(0, "ESDMA1", "come to 118.100")],
          str(r["reached_the_target"]))
    check("and arrives on no frequency at all, because it is not a transmission",
          all(t[0] == 0 for t in r["reached_the_target"]),
          str(r["reached_the_target"]))
    check("and nobody else sees it, not even someone on the same frequency",
          r["nobody_else_saw_it"] == [], str(r["nobody_else_saw_it"]))
    check("a callsign that is not in the flight is answered, not swallowed",
          r["unknown_callsign_answer"] ==
          [(0, "XRADIO", "nobody called NOSUCH is in this flight")],
          str(r["unknown_callsign_answer"]))
    check("and the reply comes back the same way",
          r["reply_came_back"] == [(0, "ESDMB1", "on my way")],
          str(r["reply_came_back"]))

    r = cpp["radio heights"]
    check("radio: 2.4 nm across Innsbruck's airport, both on the ground: heard", r["apron_2nm"] != [])
    check("radio: 10 nm down the valley: not -- two 3 m aerials see 7.7 nm, 1900 ft of altitude gave 107",
          r["apron_10nm"] == [], str(r["apron_10nm"]))
    check("radio: the tower reaches a 10 nm final at 3000 ft", r["tower_to_final"] != [])
    check("radio: and one on the ground 20 nm out, inside its 25 nm service volume",
          r["tower_to_20nm_on_ground"] != [])
    check("radio: a ground station reaches FL350 210 nm away, not 264",
          r["tower_to_fl350_210nm"] != [] and r["tower_to_fl350_264nm"] == [])
    heard = [n for _f, n, _b in r["tower_hears"]]
    check("radio: and hears both of them back", "OERIM1" in heard and "DLH4AB" in heard, str(heard))
    check("radio: the service volume is the station's -- a pilot at the airport does not hear the one 20 nm out",
          not any(n == "OERIM1" for _f, n, _b in r["pilot_at_airport_hears"]), str(r["pilot_at_airport_hears"]))
    check("radio: the traffic carries the height above the ground each is routed by",
          dict(r["traffic_agl"]).get("OEFIN1") == 914.4 and dict(r["traffic_agl"]).get("OELWA2") == 0.0,
          str(r["traffic_agl"]))

    r = cpp["v5 client"]
    check("v5: a v5 client logs in next to v6 ones", r["logged_in"] == [True] * 4, str(r["logged_in"]))
    check("v5: everything it is sent has a v5 header", r["versions"] == [5], str(r["versions"]))
    check("v5: its traffic comes in 108-byte v5 entries, the v6 pilot in it",
          r["v5_sized_entries"] and "OELWA1" in r["sees"], str(r["sees"]))
    check("v5: and none of v6's own packets", P.PT_ATC_LIST not in r["kinds"], str(r["kinds"]))
    check("v5: it hears the v6 pilot next door, and is heard back",
          r["old_hears_new"] != [] and r["new_hears_old"] != [])
    check("v5: on the ground with no height sent it counts 0 above it: 10 nm down the valley, nothing",
          r["far5_hears_new"] == [], str(r["far5_hears_new"]))
    check("v5: in the air it counts its altitude, as before: 9000 ft 114 nm away still hears",
          r["up5_hears_new"] != [])
    check("v5: a v4 client is still refused, in a v4 header",
          r["v4_rejected"] == [(4, P.RJ_VERSION)], str(r["v4_rejected"]))

    r = cpp["ducting"]
    check("ducting: 80 nm over the sea at 500 ft is past the horizon", r["before"] == [], str(r["before"]))
    check("ducting: warm, still weather and the switch on: the ATC list says the air ducts",
          bool((r["flags_warm"] or 0) & P.AL_DUCTING), str(r["flags_warm"]))
    check("ducting: and the 80 nm get through", r["warm"] != [])
    check("ducting: cold weather ends it", r["flags_cold"] == 0 and r["cold"] == [],
          f"{r['flags_cold']} {r['cold']}")
    r = cpp["ducting off"]
    check("ducting: off unless asked -- the same weather changes nothing",
          not (r["flags_warm"] or 0) & P.AL_DUCTING and r["warm"] == [], f"{r['flags_warm']} {r['warm']}")

    print()
    if failures:
        print(f"{len(failures)} FAILED: {', '.join(failures)}")
        return 1
    print("both servers behave identically, and correctly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
