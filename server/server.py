#!/usr/bin/env python3
"""XRadio relay server.

A single asyncio UDP endpoint that:
  * tracks logged-in sessions (one per client address),
  * broadcasts nearby traffic to every client ~5 times a second,
  * routes text and voice packets to whoever is listening on that frequency
    and is within VHF line-of-sight range.

Run:  python3 server.py --host 0.0.0.0 --port 49100
"""

import argparse
import asyncio
import logging
import math
import os
import time
from dataclasses import dataclass, field

import protocol as P

LOG = logging.getLogger("xradio")

# --- tuning knobs -----------------------------------------------------------
TRAFFIC_HZ = 10.0           # traffic broadcast rate: twice the clients' report rate, so
                            # every report is forwarded within 100 ms instead of being
                            # sampled by a second 5 Hz clock (which duplicates and skips)
TRAFFIC_RANGE_NM = 80.0     # how far away other aircraft are still sent
SESSION_TIMEOUT_S = 15.0    # drop a client we have not heard from
TX_HOLD_S = 0.4             # how long txActive stays set after the last voice frame
MAX_ENTRIES_PER_PACKET = 13  # 13 * 104 + 16 < 1400 bytes
MAX_TEXT_BYTES = 200        # cap relayed text so one client cannot spam huge frames
MAX_VOICE_BYTES = 512       # one 20 ms Opus frame at 24 kbit/s is ~60 bytes


@dataclass
class Session:
    sid: int
    addr: tuple
    callsign: str
    ac_icao: str
    last_seen: float
    livery: str = ""
    weather_source: bool = False   # claimed the flight's weather and time
    lat: float = 0.0
    lon: float = 0.0
    alt_m: float = 0.0
    heading: float = 0.0
    pitch: float = 0.0
    roll: float = 0.0
    gs_ms: float = 0.0
    gear: float = 0.0
    flap: float = 0.0
    com1: int = 0
    com2: int = 0
    lights: int = 0
    on_ground: int = 1
    tx_radio: int = P.TX_NONE
    rx_mask: int = P.RX_COM1
    last_voice: float = 0.0
    has_position: bool = False
    voice_seq_out: int = field(default=0)
    time_ms: int = 0            # the client's own timestamp, relayed untouched
    track: float = 0.0
    vs_ms: float = 0.0

    @property
    def alt_ft(self) -> float:
        return self.alt_m * 3.28084

    def listening_on(self, freq_khz: int) -> bool:
        """True if this session has that frequency tuned on a receiving radio."""
        if freq_khz == 0:
            return False
        if (self.rx_mask & P.RX_COM1) and self.com1 == freq_khz:
            return True
        if (self.rx_mask & P.RX_COM2) and self.com2 == freq_khz:
            return True
        return False

    def tx_freq(self) -> int:
        if self.tx_radio == P.TX_COM1:
            return self.com1
        if self.tx_radio == P.TX_COM2:
            return self.com2
        return 0


def _clean(s: str, maxlen: int) -> str:
    """Keep printable ASCII only: these strings end up on other pilots' screens."""
    return "".join(c for c in s if 32 <= ord(c) < 127)[:maxlen].strip()


def distance_nm(a: Session, b: Session) -> float:
    """Great-circle distance in nautical miles (haversine)."""
    r_nm = 3440.065
    p1, p2 = math.radians(a.lat), math.radians(b.lat)
    dp = p2 - p1
    dl = math.radians(b.lon - a.lon)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r_nm * math.asin(min(1.0, math.sqrt(h)))


def radio_range_nm(a: Session, b: Session) -> float:
    """VHF line-of-sight horizon between two aircraft, in NM.

    The classic approximation: 1.23 * (sqrt(h1_ft) + sqrt(h2_ft)).
    Clamped at the bottom so two aircraft on the same apron can still talk.
    """
    los = 1.23 * (math.sqrt(max(0.0, a.alt_ft)) + math.sqrt(max(0.0, b.alt_ft)))
    return max(15.0, los)


def in_radio_range(a: Session, b: Session) -> bool:
    return distance_nm(a, b) <= radio_range_nm(a, b)


class XRadioServer(asyncio.DatagramProtocol):
    def __init__(self, password: str = ""):
        self.password = password       # empty: anyone may join
        self.transport = None
        self.sessions: dict[tuple, Session] = {}   # addr -> Session
        self.by_sid: dict[int, Session] = {}
        self._next_sid = 1
        self._t0 = time.monotonic()
        # Whose sky everyone else flies in. The first pilot to claim it keeps
        # it until they leave; a second claimant would mean the weather
        # flickered between two sims.
        self._weather_sid = 0

    # -- asyncio plumbing ---------------------------------------------------
    def connection_made(self, transport):
        self.transport = transport
        LOG.info("listening on %s", transport.get_extra_info("sockname"))

    def datagram_received(self, data: bytes, addr: tuple):
        parsed = P.unpack_header(data)
        if parsed is None:
            return                      # not ours, or truncated -- ignore silently
        ptype, version, _plen, sid, payload = parsed
        if version != P.PROTO_VERSION:
            # Tell a login from another version why it is getting nowhere;
            # the header is the same in every version, so the reply can at
            # least be seen. Anything else from a mismatched client is dropped.
            if ptype == P.PT_LOGIN:
                self._send(addr, P.PT_LOGIN_REJECT, 0, P.LOGIN_REJECT.pack(P.RJ_VERSION, 0))
            return
        try:
            self._dispatch(ptype, sid, payload, addr)
        except Exception:               # never let one bad packet kill the server
            LOG.exception("error handling packet type %s from %s", ptype, addr)

    def error_received(self, exc):
        LOG.debug("udp error: %s", exc)

    def _send(self, addr: tuple, ptype: int, sid: int, payload: bytes = b""):
        self.transport.sendto(P.pack(ptype, sid, payload), addr)

    def _now_ms(self) -> int:
        return int((time.monotonic() - self._t0) * 1000)

    # -- packet handlers ----------------------------------------------------
    def _dispatch(self, ptype, sid, payload, addr):
        if ptype == P.PT_LOGIN:
            return self._on_login(payload, addr)

        s = self.sessions.get(addr)
        if s is None:
            return                      # everything else requires a login first
        # The session id is only ever sent to the address that logged in, so
        # requiring it here means an off-path spoofer has to guess it before
        # it can drive traffic at someone.
        if sid != s.sid:
            return
        s.last_seen = time.monotonic()

        if ptype == P.PT_POSITION:
            self._on_position(s, payload)
        elif ptype == P.PT_TEXT:
            self._on_text(s, payload)
        elif ptype == P.PT_VOICE:
            self._on_voice(s, payload)
        elif ptype == P.PT_PING:
            self._send(addr, P.PT_PONG, s.sid)
        elif ptype == P.PT_WEATHER:
            self._on_weather(s, payload)
        elif ptype == P.PT_LOGOUT:
            self._drop(s, "logout")

    def _on_login(self, payload, addr):
        if len(payload) < P.LOGIN.size:
            return
        (callsign_raw, icao_raw, proto_ver, flags,
         livery_raw, password_raw) = P.LOGIN.unpack_from(payload, 0)
        if proto_ver != P.PROTO_VERSION:
            LOG.warning("rejecting %s: protocol v%s", addr, proto_ver)
            self._send(addr, P.PT_LOGIN_REJECT, 0, P.LOGIN_REJECT.pack(P.RJ_VERSION, 0))
            return
        if self.password and P.cstr(password_raw) != self.password:
            LOG.warning("rejecting %s: wrong password", addr)
            self._send(addr, P.PT_LOGIN_REJECT, 0, P.LOGIN_REJECT.pack(P.RJ_PASSWORD, 0))
            return

        callsign = _clean(P.cstr(callsign_raw), 15)
        ac_icao = _clean(P.cstr(icao_raw), 7)
        livery = _clean(P.cstr(livery_raw), 15)

        old = self.sessions.pop(addr, None)
        if old:
            self.by_sid.pop(old.sid, None)

        sid = self._next_sid
        self._next_sid += 1
        s = Session(
            sid=sid,
            addr=addr,
            callsign=callsign or f"UNK{sid}",
            ac_icao=ac_icao or "ZZZZ",
            last_seen=time.monotonic(),
            livery=livery,
            weather_source=bool(flags & P.LF_WEATHER_SOURCE),
        )
        self.sessions[addr] = s
        self.by_sid[sid] = s
        if s.weather_source and self._weather_sid == 0:
            self._weather_sid = sid
            LOG.info("weather and time now come from %s", s.callsign)
        LOG.info("login: %s (%s) sid=%d from %s", s.callsign, s.ac_icao, sid, addr)
        self._send(addr, P.PT_LOGIN_ACK, sid, P.LOGIN_ACK.pack(sid, self._now_ms()))

    def _on_position(self, s: Session, payload):
        if len(payload) < P.POSITION.size:
            return
        (lat, lon, alt_m, heading, pitch, roll, gs_ms,
         gear, flap, com1, com2,
         lights, on_ground, tx_radio, rx_mask,
         time_ms, track, vs_ms) = P.POSITION.unpack_from(payload, 0)

        # A client can send anything. NaN or a wild coordinate would be relayed
        # to everyone else and poison their renderer, and it breaks the
        # distance maths here too, so a bad report is dropped at the door.
        if not (math.isfinite(lat) and -90.0 <= lat <= 90.0):
            return
        if not (math.isfinite(lon) and -180.0 <= lon <= 180.0):
            return
        if not (math.isfinite(alt_m) and -1000.0 <= alt_m <= 40000.0):
            return
        if not all(math.isfinite(v) for v in (heading, pitch, roll, gs_ms, gear, flap,
                                              track, vs_ms)):
            return
        if not -200.0 <= vs_ms <= 200.0:
            return
        if not 0.0 <= gs_ms <= 1500.0:
            return

        s.lat, s.lon, s.alt_m = lat, lon, alt_m
        s.heading = heading % 360.0
        s.pitch, s.roll = pitch, roll
        s.gs_ms = gs_ms
        s.gear = min(1.0, max(0.0, gear))
        s.flap = min(1.0, max(0.0, flap))
        s.com1, s.com2 = com1, com2
        s.lights, s.on_ground = lights, on_ground
        s.tx_radio, s.rx_mask = tx_radio, rx_mask
        s.time_ms, s.track, s.vs_ms = time_ms, track % 360.0, vs_ms
        s.has_position = True

    def _on_text(self, s: Session, payload):
        if len(payload) < P.TEXT_HDR.size:
            return
        want_freq, _from_sid, _from, text_len = P.TEXT_HDR.unpack_from(payload, 0)
        text = payload[P.TEXT_HDR.size:P.TEXT_HDR.size + min(text_len, MAX_TEXT_BYTES)]
        if not text:
            return

        # Text does not need the PTT held down, so it is sent on whichever
        # radio the client names -- but only if that radio is really tuned
        # there, so nobody can transmit on a frequency they are not on.
        freq = 0
        if want_freq and want_freq in (s.com1, s.com2):
            freq = want_freq
        elif want_freq == 0:
            freq = s.tx_freq() or s.com1
        if freq == 0:
            return
        out_payload = P.TEXT_HDR.pack(freq, s.sid, P.pad(s.callsign, 16), len(text)) + text
        for peer in self._listeners(s, freq):
            self._send(peer.addr, P.PT_TEXT, peer.sid, out_payload)
        LOG.info("text %s on %.3f: %s", s.callsign, freq / 1000.0,
                 text.decode("utf-8", "replace"))

    def _on_voice(self, s: Session, payload):
        if len(payload) < P.VOICE_HDR.size:
            return
        want_freq, _from_sid, seq, opus_len = P.VOICE_HDR.unpack_from(payload, 0)
        opus = payload[P.VOICE_HDR.size:P.VOICE_HDR.size + min(opus_len, MAX_VOICE_BYTES)]
        if not opus:
            return

        # Same rule as text: the client names the radio it is keying and must
        # really be tuned there. Relying on tx_radio from the last position
        # report would drop the first ~200 ms of every transmission.
        freq = 0
        if want_freq and want_freq in (s.com1, s.com2):
            freq = want_freq
        elif want_freq == 0:
            freq = s.tx_freq()
        if freq == 0:
            return

        now = time.monotonic()
        if now - s.last_voice > 1.0:
            LOG.info("voice %s keyed on %.3f", s.callsign, freq / 1000.0)
        s.last_voice = now
        out_payload = P.VOICE_HDR.pack(freq, s.sid, seq, len(opus)) + opus
        for peer in self._listeners(s, freq):
            self._send(peer.addr, P.PT_VOICE, peer.sid, out_payload)

    def _listeners(self, sender: Session, freq_khz: int):
        """Everyone except the sender who has freq tuned and is in range."""
        for peer in self.sessions.values():
            if peer.sid == sender.sid or not peer.listening_on(freq_khz):
                continue
            if sender.has_position and peer.has_position and not in_radio_range(sender, peer):
                continue
            yield peer

    # -- periodic work ------------------------------------------------------
    async def run_traffic_loop(self):
        period = 1.0 / TRAFFIC_HZ
        while True:
            await asyncio.sleep(period)
            try:
                self._reap()
                self._broadcast_traffic()
            except Exception:
                LOG.exception("traffic loop error")

    def _reap(self):
        now = time.monotonic()
        for s in [s for s in self.sessions.values() if now - s.last_seen > SESSION_TIMEOUT_S]:
            self._drop(s, "timeout")

    def _on_weather(self, s: Session, payload):
        """One sim decides the sky; the rest are told about it.

        Relayed untouched and only from the session that claimed it at login,
        so a joining pilot cannot quietly move everyone else's weather."""
        if len(payload) != P.WEATHER.size:
            return
        if self._weather_sid == 0 and s.weather_source:
            self._weather_sid = s.sid          # claimed late (the host rejoined)
        if s.sid != self._weather_sid:
            return
        for other in list(self.sessions.values()):
            if other.sid != s.sid:
                self._send(other.addr, P.PT_WEATHER, other.sid, payload)

    def _drop(self, s: Session, why: str):
        self.sessions.pop(s.addr, None)
        self.by_sid.pop(s.sid, None)
        if self._weather_sid == s.sid:
            self._weather_sid = 0      # the next claimant may have it
        LOG.info("drop %s (sid=%d): %s", s.callsign, s.sid, why)

    def _broadcast_traffic(self):
        now = time.monotonic()
        live = [s for s in self.sessions.values() if s.has_position]
        for me in live:
            others = []
            for other in live:
                if other.sid == me.sid:
                    continue
                if distance_nm(me, other) > TRAFFIC_RANGE_NM:
                    continue
                others.append(other)
            # nearest first, so the packet cap drops the far ones
            others.sort(key=lambda o: distance_nm(me, o))
            self._send_traffic(me, others[:MAX_ENTRIES_PER_PACKET], now)

    def _send_traffic(self, me: Session, others: list, now: float):
        parts = [P.TRAFFIC_HDR.pack(len(others), 0)]
        for o in others:
            tx_active = 0
            if now - o.last_voice < TX_HOLD_S:
                f = o.tx_freq()
                tx_active = 1 if (f and me.listening_on(f)) else 0
            parts.append(P.TRAFFIC_ENTRY.pack(
                o.sid, P.pad(o.callsign, 16), P.pad(o.ac_icao, 8),
                o.lat, o.lon, o.alt_m, o.heading, o.pitch, o.roll,
                o.gs_ms, o.gear, o.flap,
                o.lights, o.on_ground, tx_active, 0,
                o.time_ms, o.track, o.vs_ms,
                P.pad(o.livery, 16)))
        self._send(me.addr, P.PT_TRAFFIC, me.sid, b"".join(parts))


async def main():
    ap = argparse.ArgumentParser(description="XRadio relay server")
    ap.add_argument("--host", default=os.environ.get("XRADIO_HOST", "0.0.0.0"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("XRADIO_PORT", "49100")))
    ap.add_argument("--password", default=os.environ.get("XRADIO_PASSWORD", ""),
                    help="flight password pilots must give to join (default: none)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)-7s %(message)s",
    )

    loop = asyncio.get_running_loop()
    server = XRadioServer(password=args.password)
    if args.password:
        LOG.info("a flight password is required to join")
    transport, _ = await loop.create_datagram_endpoint(
        lambda: server, local_addr=(args.host, args.port))
    try:
        await server.run_traffic_loop()
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
