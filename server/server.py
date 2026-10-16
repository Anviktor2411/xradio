#!/usr/bin/env python3
"""XRadio relay server.

A single asyncio UDP endpoint that:
  * tracks logged-in sessions (one per client address),
  * broadcasts nearby traffic to every client ~5 times a second,
  * routes text and voice packets to whoever is listening on that frequency
    and is within VHF line-of-sight range.

Run:  python3 server.py --host 0.0.0.0 --port 49100
      python3 server.py --port 49100 --dashboard      # a live picture instead of a log
"""

import argparse
import asyncio
import collections
import logging
import math
import os
import shutil
import socket
import sys
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
# From the struct sizes, not written down: it was written down once and went
# quietly wrong the next time an entry grew.
MAX_ENTRIES_PER_PACKET = P.MAX_TRAFFIC_ENTRIES
# More aircraft in range than fit in one packet go in several, so the packet
# size stops deciding how many aeroplanes a pilot can see. This caps the whole
# lot, nearest first: a bound on the work one crowded client can ask for.
MAX_ENTRIES_TOTAL = 60
MAX_TEXT_BYTES = 200        # cap relayed text so one client cannot spam huge frames
ROSTER_INTERVAL_S = 5.0     # everyone in the flight, to everyone, this often and on every join/leave
MAX_VOICE_BYTES = 512       # one 20 ms Opus frame at 24 kbit/s is ~60 bytes
# v6. An injected aircraft the injector has stopped refreshing is gone: it
# would otherwise hang in everyone's sky after the program driving it died.
AI_TIMEOUT_S = 5.0
# ...and how many one injector may drive: the same cap one client's traffic
# packet set gets, nearest first on the way out.
MAX_AI_PER_INJECTOR = MAX_ENTRIES_TOTAL
# The ATC list goes out with the roster: whenever a station joins or leaves,
# and this often besides.
ATC_LIST_INTERVAL_S = ROSTER_INTERVAL_S


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
    squawk: int = 1200          # the four digits as they read on the panel
    xpdr_mode: int = P.XPDR_ALT
    xpdr_ident: int = 0
    # v6
    atc: bool = False           # an ATC station: never relayed as traffic, listed in PT_ATC_LIST
    injector: bool = False      # drives aircraft of its own (PT_AI_TRAFFIC)
    ai: bool = False            # this record is one of those aircraft, not a client
    owner: int = 0              # ...and this is the injector it belongs to
    fp: bytes = None            # a pilot's latest flight plan, as it will be relayed
    atis_seen: dict = field(default_factory=dict)   # icao -> letter already sent to this pilot
    kind: int = P.ATC_OTHER     # a station's facility, from its callsign
    proto: int = P.PROTO_VERSION   # what the client speaks: a v5 one gets v5 packets
    # v6: the radio model (docs/radio.md)
    agl_m: float = 0.0          # height above the terrain as reported (a station: its antenna)
    agl_known: bool = False     # the report carried it; a v5-sized one does not
    svc_range_nm: int = 0       # a station's service volume as it asked, 0 = by facility
    svc_ceiling_ft: int = 0

    @property
    def alt_ft(self) -> float:
        return self.alt_m * 3.28084

    @property
    def radio_agl_m(self) -> float:
        """The height above the ground the radio counts for this aircraft."""
        return effective_agl_m(self.agl_known, self.agl_m, bool(self.on_ground), self.alt_m)

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


def disable_udp_reset(transport):
    """Windows only: stop a vanished client from costing us other clients' packets.

    When a datagram is sent to a port nobody listens on any more, Windows
    answers itself with an ICMP "port unreachable" and reports it on the next
    receive of the *sending* socket as WSAECONNRESET -- and that failed
    receive is a receive that did not happen, so a packet from somebody else
    is lost. A pilot who closed X-Plane while the server still relayed
    traffic to them produced a steady stream of those, and single packets
    from everyone else (a text, a flight plan) went missing until the dead
    session timed out. The C++ server switches the behaviour off with
    SIO_UDP_CONNRESET; this is the same switch, from Python."""
    if os.name != "nt":
        return
    sock = transport.get_extra_info("socket") if transport else None
    if sock is None:
        return
    # asyncio hands out a read-only wrapper; the real socket is behind it.
    # Python's socket.ioctl() knows only three control codes and this is not
    # one of them, so the call goes straight to Winsock.
    raw = getattr(sock, "_sock", sock)
    try:
        import ctypes
        ws2 = ctypes.windll.ws2_32
        ws2.WSAIoctl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                                 ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p,
                                 ctypes.c_void_p]
        ws2.WSAIoctl.restype = ctypes.c_int
        sio_udp_connreset = 0x9800000C          # _WSAIOW(IOC_VENDOR, 12)
        off = ctypes.c_ulong(0)
        returned = ctypes.c_ulong(0)
        ws2.WSAIoctl(raw.fileno(), sio_udp_connreset, ctypes.byref(off), ctypes.sizeof(off),
                     None, 0, ctypes.byref(returned), None, None)
    except (OSError, AttributeError, ValueError):
        pass


def distance_nm(a: Session, b: Session) -> float:
    """Great-circle distance in nautical miles (haversine)."""
    return distance_deg_nm(a.lat, a.lon, b.lat, b.lon)


def distance_deg_nm(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r_nm = 3440.065
    p1, p2 = lat1 * math.pi / 180.0, lat2 * math.pi / 180.0
    dp = p2 - p1
    dl = (lon2 - lon1) * math.pi / 180.0
    h = math.sin(dp / 2) * math.sin(dp / 2) + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) * math.sin(dl / 2)
    return 2 * r_nm * math.asin(min(1.0, math.sqrt(h)))


# --- the radio model -----------------------------------------------------------
# How far a transmission reaches and how strong it is when it gets there --
# docs/radio.md has the formulas with worked examples. plugin/src/radio.h is
# the same model line for line (the built-in server routes by it and the
# plugin turns it into noise and break-up), and tools/check_sizes.py compares
# the two on a table of cases: two servers deciding differently would be a
# station heard in one flight and silent in another.
FT_PER_M = 3.28084
NM_KM = 1.852
AIRCRAFT_ANTENNA_M = 3.0      # a blade aerial on the fuselage, ~10 ft
HORIZON_K = 1.23              # nm per sqrt(ft): the radio horizon over an earth bent to 4/3
RANGE_FLOOR_NM = 5.0          # this close always works: one airport is never bigger
# An ATC station that does not say: its antenna above the terrain around it
# (m), and its service volume -- radius (nm) and ceiling (ft above it) --
# inside which it is heard whatever the horizon says, like a facility whose
# antennas are placed to cover its airspace.
STATION_DEFAULTS = {
    P.ATC_DEL: (30.0, 10.0, 2000.0),
    P.ATC_GND: (30.0, 10.0, 2000.0),
    P.ATC_TWR: (45.0, 25.0, 4000.0),
    P.ATC_APP: (300.0, 60.0, 25000.0),
    P.ATC_CTR: (600.0, 250.0, 65000.0),   # a centre's whole area
    P.ATC_ATIS: (45.0, 60.0, 20000.0),
}
STATION_UNKNOWN = (30.0, 0.0, 0.0)
DUCT_FACTOR = 2.0             # tropospheric ducting: low links reach this much further
DUCT_MAX_HEIGHT_FT = 6000.0
TX_AIRCRAFT_DBM = 40.0        # 10 W
TX_STATION_DBM = 43.9794      # 25 W
NOISE_DBM = -113.0            # 25 kHz channel, 7 dB noise figure
SNR_ZERO_DB = 3.0             # nothing worth hearing at or below
SNR_CLEAN_DB = 33.0           # clean at or above
FADE_FROM = 0.6               # clean to this share of the horizon, nothing at it
SERVICE_FADE_FROM = 0.9       # a service volume is full strength to its last tenth


@dataclass
class Node:
    """One end of a link, as the model needs it."""
    lat: float
    lon: float
    alt_msl_m: float
    height_ft: float            # its aerial above the terrain around it
    atc: bool = False
    range_nm: float = 0.0       # service volume (stations)
    ceiling_ft: float = 0.0


def aircraft_height_ft(agl_m: float) -> float:
    return (max(0.0, agl_m) + AIRCRAFT_ANTENNA_M) * FT_PER_M


def effective_agl_m(known: bool, agl_m: float, on_ground: bool, alt_msl_m: float) -> float:
    """What the radio counts for an aircraft: its report, or for one that sends
    none (v5, a 108-byte injected entry) 0 on the ground and its altitude in
    the air -- what every version before v6 used."""
    if known:
        return agl_m
    return 0.0 if on_ground else alt_msl_m


def horizon_nm(h1_ft: float, h2_ft: float) -> float:
    los = HORIZON_K * (math.sqrt(max(0.0, h1_ft)) + math.sqrt(max(0.0, h2_ft)))
    return max(RANGE_FLOOR_NM, los)


def horizon_for(a: Node, b: Node, ducting: bool) -> float:
    """The horizon between these two, longer in a duct."""
    r = horizon_nm(a.height_ft, b.height_ft)
    if ducting and a.height_ft <= DUCT_MAX_HEIGHT_FT and b.height_ft <= DUCT_MAX_HEIGHT_FT:
        r *= DUCT_FACTOR
    return r


def service_range_for(a: Node, b: Node) -> float:
    """A station's service volume radius when the other end is an aircraft
    below its ceiling; 0 otherwise."""
    if a.atc == b.atc:
        return 0.0
    st, ac = (a, b) if a.atc else (b, a)
    if st.range_nm > 0.0 and ac.alt_msl_m * FT_PER_M <= st.alt_msl_m * FT_PER_M + st.ceiling_ft:
        return st.range_nm
    return 0.0


def coverage_nm(a: Node, b: Node, ducting: bool) -> float:
    """The horizon, or a station's service volume for an aircraft inside it,
    whichever is further."""
    return max(horizon_for(a, b, ducting), service_range_for(a, b))


def smoothstep01(t: float) -> float:
    if t <= 0.0:
        return 0.0
    if t >= 1.0:
        return 1.0
    return t * t * (3.0 - 2.0 * t)


def airband_mhz(freq_khz: int) -> float:
    return min(160.0, max(100.0, freq_khz / 1000.0))


def fspl_db(mhz: float, km: float) -> float:
    """Free space path loss: 32.45 + 20 log10(f MHz) + 20 log10(d km)."""
    return 32.45 + 20.0 * math.log10(mhz) + 20.0 * math.log10(max(km, 0.05))


def link(tx: Node, rx: Node, freq_khz: int, ducting: bool, terrain_loss_db: float = 0.0):
    """(quality 0..1, level dBm, distance nm, coverage nm) from tx to rx.
    Quality above 0 is what the server relays."""
    dist = distance_deg_nm(tx.lat, tx.lon, rx.lat, rx.lon)
    rh, rs = horizon_for(tx, rx, ducting), service_range_for(tx, rx)
    cov = max(rh, rs)
    if dist >= cov:
        return 0.0, -200.0, dist, cov
    fade_h = 1.0 - smoothstep01((dist / rh - FADE_FROM) / (1.0 - FADE_FROM))
    fade_s = (1.0 - smoothstep01((dist / rs - SERVICE_FADE_FROM) / (1.0 - SERVICE_FADE_FROM))
              if rs > 0.0 else 0.0)
    fade = max(fade_h, fade_s)
    p_rx = ((TX_STATION_DBM if tx.atc else TX_AIRCRAFT_DBM)
            - fspl_db(airband_mhz(freq_khz), dist * NM_KM) - terrain_loss_db)
    snr = p_rx - NOISE_DBM
    q = smoothstep01((snr - SNR_ZERO_DB) / (SNR_CLEAN_DB - SNR_ZERO_DB)) * fade
    level = p_rx + 20.0 * math.log10(max(fade, 1e-3))
    return q, level, dist, cov


def station_radio(s: Session):
    """A station's antenna height (m), service radius (nm) and ceiling (ft):
    what it sent, or its facility's defaults."""
    ant, rng, ceil = STATION_DEFAULTS.get(s.kind, STATION_UNKNOWN)
    if s.agl_known and s.agl_m > 0.0:
        ant = s.agl_m
    if s.svc_range_nm > 0:
        rng = float(s.svc_range_nm)
    if s.svc_ceiling_ft > 0:
        ceil = float(s.svc_ceiling_ft)
    return ant, rng, ceil


def node_of(s: Session) -> Node:
    if s.atc:
        ant, rng, ceil = station_radio(s)
        return Node(s.lat, s.lon, s.alt_m, ant * FT_PER_M, True, rng, ceil)
    return Node(s.lat, s.lon, s.alt_m, aircraft_height_ft(s.radio_agl_m), False)


def in_radio_range(tx: Session, rx: Session, freq_khz: int, ducting: bool = False) -> bool:
    return link(node_of(tx), node_of(rx), freq_khz, ducting)[0] > 0.0


def ducting_from(w) -> bool:
    """Tropospheric ducting from the flight's weather (a WEATHER tuple): warm,
    high pressure, little wind, dry -- and then a third of the hours, picked
    from the sim's date and hour so every server agrees which."""
    zulu, date_days, slp, slt, rain, wind0 = w[1], w[2], w[4], w[5], w[6], w[35]
    if not (slt >= 20.0 and slp >= 101500.0 and wind0 <= 6.0 and rain < 0.1):
        return False
    hour = date_days * 24 + math.floor(zulu / 3600.0)
    return ((hour * 2654435761) & 0xFFFFFFFF) < 0x59999999


class XRadioServer(asyncio.DatagramProtocol):
    def __init__(self, password: str = "", ducting: bool = False):
        self.password = password       # empty: anyone may join
        # Tropospheric ducting (docs/radio.md): off unless whoever runs the
        # server asks for it. When on, the flight's shared weather decides --
        # warm and still, then a third of the hours -- and the ATC list tells
        # every receiver, so the radios hear what the server now relays.
        self.ducting_enabled = ducting
        self.ducting = False
        self.transport = None
        self.sessions: dict[tuple, Session] = {}   # addr -> Session
        self.by_sid: dict[int, Session] = {}
        self._next_sid = 1
        self._t0 = time.monotonic()
        # Everything that arrived and everything that went out, for the
        # dashboard's packets-per-second line.
        self.packets_in = 0
        self.packets_out = 0
        # Whose sky everyone else flies in. The first pilot to claim it keeps
        # it until they leave; a second claimant would mean the weather
        # flickered between two sims.
        self._weather_sid = 0
        # The roster: who is in the flight, for the @ list in the Say field.
        self._roster_dirty = False
        self._last_roster = 0.0
        self._roster_epoch = 0
        # v6. Aircraft driven by injectors, by their flight-wide id; the ATIS
        # texts on the air, by airport; and whether the ATC list changed.
        self.ai: dict[int, Session] = {}
        self.atis: dict[str, dict] = {}
        self._atc_dirty = False
        self._last_atc = 0.0

    # -- asyncio plumbing ---------------------------------------------------
    def connection_made(self, transport):
        self.transport = transport
        LOG.info("listening on %s", transport.get_extra_info("sockname"))

    def datagram_received(self, data: bytes, addr: tuple):
        self.packets_in += 1
        parsed = P.unpack_header(data)
        if parsed is None:
            return                      # not ours, or truncated -- ignore silently
        ptype, version, _plen, sid, payload = parsed
        if not P.MIN_PROTO_VERSION <= version <= P.PROTO_VERSION:
            # Tell a login from another version why it is getting nowhere, in
            # its own header version -- an older client drops any other.
            # Anything else from a mismatched client is dropped.
            if ptype == P.PT_LOGIN:
                self._send(addr, P.PT_LOGIN_REJECT, 0, P.LOGIN_REJECT.pack(P.RJ_VERSION, 0), version)
            return
        try:
            self._dispatch(ptype, version, sid, payload, addr)
        except Exception:               # never let one bad packet kill the server
            LOG.exception("error handling packet type %s from %s", ptype, addr)

    def error_received(self, exc):
        LOG.debug("udp error: %s", exc)

    def _send(self, addr: tuple, ptype: int, sid: int, payload: bytes = b"", version: int = 0):
        """`version` 0: the header version the receiver speaks (a v5 client
        drops a packet with any other in it)."""
        if not version:
            to = self.sessions.get(addr)
            version = to.proto if to is not None else P.PROTO_VERSION
        self.packets_out += 1
        self.transport.sendto(P.pack(ptype, sid, payload, version), addr)

    def _now_ms(self) -> int:
        return int((time.monotonic() - self._t0) * 1000)

    # -- packet handlers ----------------------------------------------------
    def _dispatch(self, ptype, version, sid, payload, addr):
        if ptype == P.PT_LOGIN:
            return self._on_login(payload, addr, version)

        s = self.sessions.get(addr)
        if s is None:
            return                      # everything else requires a login first
        # The session id is only ever sent to the address that logged in, so
        # requiring it here means an off-path spoofer has to guess it before
        # it can drive traffic at someone. And a session speaks the one
        # version it logged in with.
        if sid != s.sid or version != s.proto:
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
        elif ptype == P.PT_AI_TRAFFIC:
            self._on_ai_traffic(s, payload)
        elif ptype == P.PT_FLIGHTPLAN:
            self._on_flightplan(s, payload)
        elif ptype == P.PT_ATIS:
            self._on_atis(s, payload)
        elif ptype == P.PT_LOGOUT:
            self._drop(s, "logout")

    def _on_login(self, payload, addr, version=P.PROTO_VERSION):
        if len(payload) < P.LOGIN.size:
            return
        (callsign_raw, icao_raw, proto_ver, flags,
         livery_raw, password_raw) = P.LOGIN.unpack_from(payload, 0)
        if proto_ver != version or not P.MIN_PROTO_VERSION <= proto_ver <= P.PROTO_VERSION:
            LOG.warning("rejecting %s: protocol v%s", addr, proto_ver)
            self._send(addr, P.PT_LOGIN_REJECT, 0, P.LOGIN_REJECT.pack(P.RJ_VERSION, 0), version)
            return
        if self.password and P.cstr(password_raw) != self.password:
            LOG.warning("rejecting %s: wrong password", addr)
            self._send(addr, P.PT_LOGIN_REJECT, 0, P.LOGIN_REJECT.pack(P.RJ_PASSWORD, 0), version)
            return
        if proto_ver < 6:
            flags &= P.LF_WEATHER_SOURCE    # the station and injector bits are v6's

        callsign = _clean(P.cstr(callsign_raw), 15)
        ac_icao = _clean(P.cstr(icao_raw), 7)
        livery = _clean(P.cstr(livery_raw), 15)

        # A re-login from the same address replaces the old session, and
        # everything that pointed at the old one lets go of it -- the weather
        # claim above all. It used to survive here, naming a session that no
        # longer existed, so a host whose login ack was lost, or who logged
        # in again after a network stall, could never share the sky again
        # and nor could anybody else.
        old = self.sessions.get(addr)
        if old is not None:
            self._drop(old, "re-login")

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
            atc=bool(flags & P.LF_ATC),
            injector=bool(flags & P.LF_INJECTOR),
            proto=proto_ver,
        )
        s.kind = P.atc_kind(s.callsign) if s.atc else P.ATC_OTHER
        self.sessions[addr] = s
        self.by_sid[sid] = s
        if s.weather_source and self._weather_sid == 0:
            self._weather_sid = sid
            LOG.info("weather and time now come from %s", s.callsign)
        LOG.info("login: %s (%s) sid=%d from %s%s%s%s", s.callsign, s.ac_icao, sid, addr,
                 " [ATC]" if s.atc else "", " [injector]" if s.injector else "",
                 f" [v{proto_ver} client]" if proto_ver != P.PROTO_VERSION else "")
        self._send(addr, P.PT_LOGIN_ACK, sid, P.LOGIN_ACK.pack(sid, self._now_ms()))
        self._roster_dirty = True
        self._atc_dirty = True
        # A station that joins late still gets every plan in the flight.
        if s.atc:
            for p in self.sessions.values():
                if p.fp and p.sid != sid:
                    self._send(addr, P.PT_FLIGHTPLAN, sid, p.fp)

    def _on_position(self, s: Session, payload):
        # v6 adds the radio model's fields at the end; a v5-sized report is
        # still a position, its height above the ground then unknown.
        if len(payload) >= P.POSITION.size:
            fields = P.POSITION.unpack_from(payload, 0)
            agl_m, svc_range, svc_ceiling = fields[21:24]
            agl_known = True
        elif len(payload) >= P.POSITION_V5.size:
            fields = P.POSITION_V5.unpack_from(payload, 0)
            agl_m, svc_range, svc_ceiling, agl_known = 0.0, 0, 0, False
        else:
            return
        (lat, lon, alt_m, heading, pitch, roll, gs_ms,
         gear, flap, com1, com2,
         lights, on_ground, tx_radio, rx_mask,
         time_ms, track, vs_ms,
         squawk, xpdr_mode, xpdr_ident) = fields[:21]

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
        if not (math.isfinite(agl_m) and -1000.0 <= agl_m <= 40000.0):
            return

        if s.atc and (not s.has_position or (s.agl_m, s.agl_known, s.svc_range_nm, s.svc_ceiling_ft)
                      != (agl_m, agl_known, svc_range, svc_ceiling)):
            self._atc_dirty = True          # the list says where a station is and what it covers
        s.lat, s.lon, s.alt_m = lat, lon, alt_m
        s.agl_m, s.agl_known = agl_m, agl_known
        s.svc_range_nm, s.svc_ceiling_ft = svc_range, svc_ceiling
        s.heading = heading % 360.0
        s.pitch, s.roll = pitch, roll
        s.gs_ms = gs_ms
        s.gear = min(1.0, max(0.0, gear))
        s.flap = min(1.0, max(0.0, flap))
        if s.atc and s.com1 != com1:
            self._atc_dirty = True          # a station retuned: the list says its frequency
        s.com1, s.com2 = com1, com2
        s.lights, s.on_ground = lights, on_ground
        s.tx_radio, s.rx_mask = tx_radio, rx_mask
        s.time_ms, s.track, s.vs_ms = time_ms, track % 360.0, vs_ms
        # A squawk is four octal digits. Anything else is a client bug or a
        # client being clever, and either way it is not relayed as if real.
        s.squawk = squawk if 0 <= squawk <= 7777 and all(
            c <= "7" for c in f"{squawk:04d}") else 0
        s.xpdr_mode = xpdr_mode if xpdr_mode <= P.XPDR_TA_RA else P.XPDR_OFF
        s.xpdr_ident = 1 if xpdr_ident else 0
        s.has_position = True
        self._offer_atis(s)

    # -- v6: air traffic control ------------------------------------------
    def _speaker(self, s: Session, from_sid: int):
        """Who a text or voice packet is from: the session itself, or -- for an
        injector naming one of its own aircraft -- that aircraft."""
        if s.injector and P.is_ai_sid(from_sid):
            ai = self.ai.get(from_sid)
            if ai is not None and ai.owner == s.sid:
                return ai
        return s

    def _on_ai_traffic(self, s: Session, payload):
        """The aircraft an injector drives, as TRAFFIC_ENTRY records with the
        injector's own local numbers in sessionId. Validated exactly like a
        position report: the renderer at the other end trusts nothing less."""
        if not s.injector or len(payload) < P.TRAFFIC_HDR.size:
            return
        count, _ = P.TRAFFIC_HDR.unpack_from(payload, 0)
        off = P.TRAFFIC_HDR.size
        now = time.monotonic()
        mine = sum(1 for a in self.ai.values() if a.owner == s.sid)
        # v6 entries carry the aircraft's height above the ground at their
        # end; an injector that predates it sends them 108 bytes long.
        entry = P.TRAFFIC_ENTRY if len(payload) - off >= count * P.TRAFFIC_ENTRY.size else P.TRAFFIC_ENTRY_V5
        for _ in range(count):
            if off + entry.size > len(payload):
                break
            e = entry.unpack_from(payload, off)
            off += entry.size
            local = e[0] & 0xFFFF
            if local == 0:
                continue
            sid = P.ai_sid(s.sid, local)
            if e[22] & P.AI_REMOVE:
                if self.ai.pop(sid, None) is not None:
                    mine -= 1
                continue
            lat, lon, alt_m, heading, pitch, roll, gs_ms, gear, flap = e[3:12]
            track, vs_ms = e[17], e[18]
            if not (math.isfinite(lat) and -90.0 <= lat <= 90.0):
                continue
            if not (math.isfinite(lon) and -180.0 <= lon <= 180.0):
                continue
            if not (math.isfinite(alt_m) and -1000.0 <= alt_m <= 40000.0):
                continue
            if not all(math.isfinite(v) for v in (heading, pitch, roll, gs_ms, gear, flap, track, vs_ms)):
                continue
            if not -200.0 <= vs_ms <= 200.0 or not 0.0 <= gs_ms <= 1500.0:
                continue
            agl_known = entry is P.TRAFFIC_ENTRY
            agl_m = e[23] if agl_known else 0.0
            if not (math.isfinite(agl_m) and -1000.0 <= agl_m <= 40000.0):
                continue
            a = self.ai.get(sid)
            if a is None:
                if mine >= MAX_AI_PER_INJECTOR:
                    continue
                a = Session(sid=sid, addr=None, callsign="", ac_icao="", last_seen=now,
                            ai=True, owner=s.sid, tx_radio=P.TX_NONE, rx_mask=0)
                self.ai[sid] = a
                mine += 1
            a.callsign = _clean(P.cstr(e[1]), 15) or f"AI{local}"
            a.ac_icao = _clean(P.cstr(e[2]), 7) or "ZZZZ"
            a.lat, a.lon, a.alt_m = lat, lon, alt_m
            a.heading, a.pitch, a.roll, a.gs_ms = heading % 360.0, pitch, roll, gs_ms
            a.gear, a.flap = min(1.0, max(0.0, gear)), min(1.0, max(0.0, flap))
            a.lights, a.on_ground = e[12], e[13]
            a.xpdr_mode = e[15] if e[15] <= P.XPDR_TA_RA else P.XPDR_OFF
            a.time_ms, a.track, a.vs_ms = e[16], track % 360.0, vs_ms
            a.livery = _clean(P.cstr(e[19]), 15)
            squawk = e[20]
            a.squawk = squawk if 0 <= squawk <= 7777 and all(c <= "7" for c in f"{squawk:04d}") else 0
            a.xpdr_ident = 1 if e[21] else 0
            a.agl_m, a.agl_known = agl_m, agl_known
            a.has_position = True
            a.last_seen = now

    def _on_flightplan(self, s: Session, payload):
        """A pilot files; every station hears. A station answers; the pilot and
        the other stations hear. The latest copy is kept with the pilot's
        session so a station that joins later still sees it."""
        d = P.unpack_flightplan(payload)
        if d is None:
            return
        if s.atc:
            target = self.by_sid.get(d["session_id"])
            if target is None or target.atc or target.ai:
                return
            out = P.FLIGHTPLAN.pack(target.sid, P.pad(target.callsign, 16), *P.FLIGHTPLAN.unpack_from(payload, 0)[2:])
            target.fp = out
            if target.proto >= 6:           # a v5 client has no flight plans
                self._send(target.addr, P.PT_FLIGHTPLAN, target.sid, out)
            for p in self.sessions.values():
                if p.atc and p.sid != s.sid:
                    self._send(p.addr, P.PT_FLIGHTPLAN, p.sid, out)
            LOG.info("flight plan for %s: %s by %s (squawk %04d)", target.callsign,
                     {P.FP_ACK: "acknowledged", P.FP_CLEARED: "cleared", P.FP_CANCELLED: "cancelled"}.get(d["status"], d["status"]),
                     s.callsign, d["squawk"])
            return
        if s.injector:
            return                      # computer-flown aircraft do not file
        fields = list(P.FLIGHTPLAN.unpack_from(payload, 0))
        fields[0] = s.sid
        fields[1] = P.pad(s.callsign, 16)   # a pilot files as who they logged in as
        fields[5] = 0                       # only ATC assigns a squawk
        if fields[4] not in (P.FP_FILED, P.FP_CANCELLED):
            fields[4] = P.FP_FILED
        out = P.FLIGHTPLAN.pack(*fields)
        s.fp = None if fields[4] == P.FP_CANCELLED else out
        for p in self.sessions.values():
            if p.atc:
                self._send(p.addr, P.PT_FLIGHTPLAN, p.sid, out)
        LOG.info("flight plan %s: %s %s -> %s %s", "cancelled" if fields[4] == P.FP_CANCELLED else "filed",
                 s.callsign, d["dep"], d["dest"], d["route"])

    def _on_atis(self, s: Session, payload):
        """A station publishes the ATIS of an airport; it is offered to
        everyone tuned to its frequency, now and when they tune in later."""
        if not s.atc or len(payload) < P.ATIS_HDR.size:
            return
        freq, icao_raw, letter, _r, text_len = P.ATIS_HDR.unpack_from(payload, 0)
        icao = _clean(P.cstr(icao_raw), 7).upper()
        text = payload[P.ATIS_HDR.size:P.ATIS_HDR.size + min(text_len, P.MAX_ATIS_BYTES)]
        if not icao or not freq:
            return
        letter = letter.decode("ascii", "replace")
        if not ("A" <= letter <= "Z"):
            letter = "A"
        out = P.ATIS_HDR.pack(freq, P.pad(icao, 8), letter.encode(), 0, len(text)) + text
        old = self.atis.get(icao)
        self.atis[icao] = {"freq": freq, "letter": letter, "text": text, "owner": s.sid, "payload": out}
        if old is None or old["freq"] != freq:
            self._atc_dirty = True
        if old is None or old["letter"] != letter or old["text"] != text:
            LOG.info("ATIS %s information %s on %.3f (%d bytes)", icao, letter, freq / 1000.0, len(text))
            for p in self.sessions.values():
                p.atis_seen.pop(icao, None)     # the new letter goes out again
                self._offer_atis(p)

    def _offer_atis(self, s: Session):
        """Send a pilot the ATIS of every frequency they have just tuned."""
        if s.ai or s.injector or s.proto < 6:
            return
        for icao, a in self.atis.items():
            if a["owner"] == s.sid:
                continue
            if (s.com1 == a["freq"] or s.com2 == a["freq"]) and s.atis_seen.get(icao) != a["letter"]:
                s.atis_seen[icao] = a["letter"]
                self._send(s.addr, P.PT_ATIS, s.sid, a["payload"])

    @staticmethod
    def _atc_entry(sid, callsign, icao, freq, kind, where, antenna_m, range_nm, ceiling_ft):
        """One row of the ATC list: who, where it transmits from and what it
        covers -- what the server routes by, so a pilot's radio can work out
        the same signal (docs/radio.md)."""
        has = where is not None and where.has_position
        return P.ATC_ENTRY.pack(sid, P.pad(callsign, 16), P.pad(icao, 8), freq, kind,
                                P.AE_POSITION if has else 0, b"\x00\x00",
                                where.lat if has else 0.0, where.lon if has else 0.0,
                                where.alt_m if has else 0.0, antenna_m,
                                int(min(65535.0, range_nm)), int(min(65535.0, ceiling_ft)))

    def _broadcast_atc_list(self, now: float):
        """The stations online and the ATIS frequencies, to everyone -- and
        whether the flight's air is ducting tonight. Ordered by session so
        both servers produce the same bytes. A v5 client has no such list."""
        self._last_atc = now
        self._atc_dirty = False
        if not self.sessions:
            return
        entries = []
        for s in sorted(self.sessions.values(), key=lambda x: x.sid):
            if s.atc:
                ant, rng, ceil = station_radio(s)
                entries.append(self._atc_entry(s.sid, s.callsign, P.atc_icao(s.callsign), s.com1, s.kind,
                                               s, ant, rng, ceil))
        for icao in sorted(self.atis):
            a = self.atis[icao]
            owner = self.by_sid.get(a["owner"])
            ant = station_radio(owner)[0] if owner is not None else STATION_DEFAULTS[P.ATC_ATIS][0]
            _, rng, ceil = STATION_DEFAULTS[P.ATC_ATIS]
            entries.append(self._atc_entry(0, f"{icao}_ATIS", icao, a["freq"], P.ATC_ATIS, owner, ant, rng, ceil))
        entries = entries[:P.MAX_ATC_ENTRIES]
        body = P.ATC_LIST_HDR.pack(len(entries), P.AL_DUCTING if self.ducting else 0) + b"".join(entries)
        for to in list(self.sessions.values()):
            if to.proto >= 6:
                self._send(to.addr, P.PT_ATC_LIST, to.sid, body)

    def _on_text(self, s: Session, payload):
        if len(payload) < P.TEXT_HDR.size:
            return
        want_freq, from_sid, _from, text_len, to_raw = P.TEXT_HDR.unpack_from(payload, 0)
        text = payload[P.TEXT_HDR.size:P.TEXT_HDR.size + min(text_len, MAX_TEXT_BYTES)]
        if not text:
            return
        # v6: an injector speaking as one of its aircraft. Such an aircraft
        # has no radios of its own to check against, so the frequency named
        # in the packet is the one it transmits on.
        s = self._speaker(s, from_sid)

        # A message addressed to a callsign is not a transmission: it goes to
        # that one pilot wherever they are, on no frequency at all, and nobody
        # else sees it. A pilot who cannot be found is told so, rather than
        # being left to wonder whether the message landed.
        to = _clean(P.cstr(to_raw), 15)
        if to:
            target = next((x for x in self.sessions.values() if x.callsign == to), None)
            if target is not None:
                out = P.TEXT_HDR.pack(0, s.sid, P.pad(s.callsign, 16),
                                      len(text), P.pad(to, 16)) + text
                self._send(target.addr, P.PT_TEXT, target.sid, out)
                LOG.info("direct %s -> %s: %s", s.callsign, to,
                         text.decode("utf-8", "replace"))
            else:
                why = f"nobody called {to} is in this flight".encode()
                out = P.TEXT_HDR.pack(0, 0, P.pad("XRADIO", 16),
                                      len(why), P.pad(to, 16)) + why
                self._send(s.addr, P.PT_TEXT, s.sid, out)
            return

        # Text does not need the PTT held down, so it is sent on whichever
        # radio the client names -- but only if that radio is really tuned
        # there, so nobody can transmit on a frequency they are not on.
        freq = 0
        if s.ai:
            freq = want_freq
        elif want_freq and want_freq in (s.com1, s.com2):
            freq = want_freq
        elif want_freq == 0:
            freq = s.tx_freq() or s.com1
        if freq == 0:
            return
        out_payload = P.TEXT_HDR.pack(freq, s.sid, P.pad(s.callsign, 16),
                                      len(text), P.pad("", 16)) + text
        for peer in self._listeners(s, freq):
            self._send(peer.addr, P.PT_TEXT, peer.sid, out_payload)
        LOG.info("text %s on %.3f: %s", s.callsign, freq / 1000.0,
                 text.decode("utf-8", "replace"))

    def _on_voice(self, s: Session, payload):
        if len(payload) < P.VOICE_HDR.size:
            return
        want_freq, from_sid, seq, opus_len = P.VOICE_HDR.unpack_from(payload, 0)
        opus = payload[P.VOICE_HDR.size:P.VOICE_HDR.size + min(opus_len, MAX_VOICE_BYTES)]
        if not opus:
            return
        s = self._speaker(s, from_sid)

        # Same rule as text: the client names the radio it is keying and must
        # really be tuned there. Relying on tx_radio from the last position
        # report would drop the first ~200 ms of every transmission.
        freq = 0
        if s.ai:
            freq = want_freq
            # so the [TX] flag on the aircraft follows its voice like a pilot's
            s.com1, s.tx_radio = freq, P.TX_COM1
        elif want_freq and want_freq in (s.com1, s.com2):
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
        """Everyone except the sender who has freq tuned and can receive it --
        the radio model's quality above nothing (docs/radio.md): inside the
        horizon worked out from both heights above the ground, or inside a
        station's service volume.

        Guard is the exception: it reaches everyone in range whether or not
        they have it tuned. It is the one frequency you can call somebody on
        without already knowing where they are listening, which is the whole
        reason it exists.
        """
        guard = P.is_guard(freq_khz)
        for peer in self.sessions.values():
            if peer.sid == sender.sid:
                continue
            if sender.ai and peer.sid == sender.owner:
                continue                # the program driving it said it; it knows
            if not guard and not peer.listening_on(freq_khz):
                continue
            if sender.has_position and peer.has_position and \
                    not in_radio_range(sender, peer, freq_khz, self.ducting):
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
                now = time.monotonic()
                if self._roster_dirty or now - self._last_roster >= ROSTER_INTERVAL_S:
                    self._broadcast_roster(now)
                if self._atc_dirty or now - self._last_atc >= ATC_LIST_INTERVAL_S:
                    self._broadcast_atc_list(now)
            except Exception:
                LOG.exception("traffic loop error")

    def _reap(self):
        now = time.monotonic()
        for s in [s for s in self.sessions.values() if now - s.last_seen > SESSION_TIMEOUT_S]:
            self._drop(s, "timeout")
        for sid in [k for k, a in self.ai.items() if now - a.last_seen > AI_TIMEOUT_S]:
            del self.ai[sid]

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
        if self.ducting_enabled:
            self._set_ducting(ducting_from(P.WEATHER.unpack(payload)))

    def _set_ducting(self, on: bool):
        """The flight's air, ducting or not; the ATC list carries it to every
        radio at once."""
        if on == self.ducting:
            return
        self.ducting = on
        self._atc_dirty = True
        LOG.info("tropospheric ducting %s", "begins: VHF reaches far past the horizon over the sea"
                 if on else "is over: VHF range back to the horizon")

    def _drop(self, s: Session, why: str):
        if self.sessions.pop(s.addr, None) is None:
            return
        self.by_sid.pop(s.sid, None)
        if self._weather_sid == s.sid:
            self._weather_sid = 0      # the next claimant may have it
            self._set_ducting(False)   # and with no shared sky, nothing says the air is layered
        self._roster_dirty = True
        # v6: its aircraft and its ATIS go with it
        if s.injector:
            for sid in [k for k, a in self.ai.items() if a.owner == s.sid]:
                del self.ai[sid]
        if s.atc:
            for icao in [k for k, a in self.atis.items() if a["owner"] == s.sid]:
                del self.atis[icao]
            self._atc_dirty = True
        LOG.info("drop %s (sid=%d): %s", s.callsign, s.sid, why)

    def _broadcast_roster(self, now: float):
        """Everyone in the flight, to everyone in the flight.

        The receiver is in its own copy and leaves itself out. Ordered by
        session so both servers produce the same bytes, and split like
        traffic when it does not fit in one packet."""
        self._last_roster = now
        self._roster_dirty = False
        if not self.sessions:
            return
        # An injector is a program, not a pilot anyone would message; its
        # aircraft are not in the roster either (they cannot read one).
        everyone = sorted((s for s in self.sessions.values() if not s.injector),
                          key=lambda s: s.sid)[:MAX_ENTRIES_TOTAL]
        self._roster_epoch = (self._roster_epoch + 1) & 0xFFFF
        per = P.MAX_ROSTER_ENTRIES
        parts = (len(everyone) + per - 1) // per
        for to in list(self.sessions.values()):
            for part in range(parts):
                chunk = everyone[part * per:(part + 1) * per]
                body = [P.ROSTER_HDR.pack(self._roster_epoch, part, parts, len(chunk), 0)]
                for o in chunk:
                    body.append(P.ROSTER_ENTRY.pack(o.sid, P.pad(o.callsign, 16),
                                                    P.pad(o.ac_icao, 8)))
                self._send(to.addr, P.PT_ROSTER, to.sid, b"".join(body))

    def _broadcast_traffic(self):
        now = time.monotonic()
        live = [s for s in self.sessions.values() if s.has_position]
        # What can be seen: pilots, and the aircraft injectors drive. An ATC
        # station is not an aircraft, and an injector does not need its own
        # aircraft back.
        visible = [s for s in live if not s.atc] + list(self.ai.values())
        for me in live:
            others = []
            for other in visible:
                if other.sid == me.sid or (other.ai and other.owner == me.sid):
                    continue
                if distance_nm(me, other) > TRAFFIC_RANGE_NM:
                    continue
                others.append(other)
            # nearest first, so the packet cap drops the far ones
            others.sort(key=lambda o: distance_nm(me, o))
            self._send_traffic(me, others[:MAX_ENTRIES_TOTAL], now)

    def _send_traffic(self, me: Session, others: list, now: float):
        """Sent in as many packets as it takes.

        The count in each header is what that packet actually carries: a
        header promising more than it holds is how a receiver ends up reading
        a neighbouring aircraft's bytes as a position.
        """
        # A v6 receiver gets each aircraft's height above the ground as well --
        # the one its radio is routed by, so its own model agrees; a v5 one
        # gets the entries it knows, which are shorter.
        v6 = me.proto >= 6
        per = MAX_ENTRIES_PER_PACKET if v6 else P.MAX_TRAFFIC_ENTRIES_V5
        for first in range(0, max(len(others), 1), per):
            chunk = others[first:first + per]
            parts = [P.TRAFFIC_HDR.pack(len(chunk), 0)]
            for o in chunk:
                tx_active = 0
                if now - o.last_voice < TX_HOLD_S:
                    f = o.tx_freq()
                    tx_active = 1 if (f and me.listening_on(f)) else 0
                fields = (o.sid, P.pad(o.callsign, 16), P.pad(o.ac_icao, 8),
                          o.lat, o.lon, o.alt_m, o.heading, o.pitch, o.roll,
                          o.gs_ms, o.gear, o.flap,
                          o.lights, o.on_ground, tx_active, o.xpdr_mode,
                          o.time_ms, o.track, o.vs_ms,
                          P.pad(o.livery, 16), o.squawk, o.xpdr_ident, 0)
                parts.append(P.TRAFFIC_ENTRY.pack(*fields, o.radio_agl_m) if v6 else P.TRAFFIC_ENTRY_V5.pack(*fields))
            self._send(me.addr, P.PT_TRAFFIC, me.sid, b"".join(parts))


# --- the dashboard ----------------------------------------------------------
class RecentEvents(logging.Handler):
    """Keeps the last few log lines, for the dashboard to show under the table."""

    def __init__(self, keep: int = 10):
        super().__init__(level=logging.INFO)
        self.lines: collections.deque = collections.deque(maxlen=keep)

    def emit(self, record):
        stamp = time.strftime("%H:%M:%S", time.localtime(record.created))
        self.lines.append(f"{stamp}  {record.getMessage()}")


class Dashboard:
    """A live picture of the flight in the terminal, redrawn once a second.

    What whoever runs the server wants to know while a flight is on: who is
    in, where they are, what they have tuned, who is talking, and whether
    packets are flowing -- without reading a log. Plain ANSI escapes, so it
    needs nothing installed and works in any terminal, Windows 10's console
    included.
    """

    def __init__(self, server: XRadioServer, port: int, recent: RecentEvents):
        self.server, self.port, self.recent = server, port, recent
        self.t0 = time.monotonic()
        self.last_t = self.t0
        self.last_in = self.last_out = 0
        self.rate_in = self.rate_out = 0.0

    async def run(self):
        if os.name == "nt":
            os.system("")                     # switches the console to ANSI escapes
        sys.stdout.write("\x1b[?25l")         # hide the cursor
        try:
            while True:
                self.draw()
                await asyncio.sleep(1.0)
        finally:
            sys.stdout.write("\x1b[?25h\nserver stopped\n")
            sys.stdout.flush()

    @staticmethod
    def _mhz(khz: int) -> str:
        return f"{khz / 1000:.3f}" if khz else "-"

    def draw(self):
        s = self.server
        now = time.monotonic()
        dt = max(1e-3, now - self.last_t)
        self.rate_in = (s.packets_in - self.last_in) / dt
        self.rate_out = (s.packets_out - self.last_out) / dt
        self.last_t, self.last_in, self.last_out = now, s.packets_in, s.packets_out

        up = int(now - self.t0)
        uptime = (f"{up // 3600}h {up % 3600 // 60:02d}m" if up >= 3600
                  else f"{up // 60}m {up % 60:02d}s")
        source = s.by_sid.get(s._weather_sid)
        lines = [
            f"XRadio relay server  ·  port {self.port}  ·  up {uptime}  ·  "
            f"{'password required' if s.password else 'no password'}",
            f"pilots {len(s.sessions)}  ·  packets in {self.rate_in:.0f}/s, out "
            f"{self.rate_out:.0f}/s  ·  sky from {source.callsign if source else 'nobody yet'}"
            + ("  ·  DUCTING: VHF reaches far over the sea" if s.ducting else ""),
            "",
            f"{'callsign':<9}{'type':<6}{'position':<22}{'alt ft':>7} {'kt':>4}  "
            f"{'COM1':<8}{'COM2':<8}{'xpdr':<6}{'radio':<6}{'seen':>5}",
        ]
        for p in sorted(s.sessions.values(), key=lambda x: x.callsign):
            if p.has_position:
                pos = (f"{abs(p.lat):.4f}{'N' if p.lat >= 0 else 'S'} "
                       f"{abs(p.lon):.4f}{'E' if p.lon >= 0 else 'W'}")
            else:
                pos = "-"
            radio = ("voice" if now - p.last_voice < TX_HOLD_S
                     else ("keyed" if p.tx_radio else ""))
            kind = "ATC" if p.atc else (f"AI x{sum(1 for a in s.ai.values() if a.owner == p.sid)}" if p.injector else p.ac_icao)
            lines.append(
                f"{p.callsign:<9}{kind:<6}{pos:<22}{p.alt_ft:>7.0f} "
                f"{p.gs_ms * 1.94384:>4.0f}  {self._mhz(p.com1):<8}{self._mhz(p.com2):<8}"
                f"{p.squawk:04d}  {radio:<6}{now - p.last_seen:>4.0f}s")
        if not s.sessions:
            lines.append("  nobody is connected")
        if s.atis:
            lines.append("ATIS: " + "  ".join(f"{k} {v['letter']} on {self._mhz(v['freq'])}" for k, v in sorted(s.atis.items())))
        lines += ["", "recent:"] + [f"  {line}" for line in self.recent.lines]
        lines += ["", "Ctrl-C stops the server"]

        width = shutil.get_terminal_size((100, 30)).columns
        sys.stdout.write("\x1b[H\x1b[2J" + "\n".join(line[:width] for line in lines) + "\n")
        sys.stdout.flush()


async def main():
    ap = argparse.ArgumentParser(description="XRadio relay server")
    ap.add_argument("--host", default=os.environ.get("XRADIO_HOST", "0.0.0.0"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("XRADIO_PORT", "49100")))
    ap.add_argument("--password", default=os.environ.get("XRADIO_PASSWORD", ""),
                    help="flight password pilots must give to join (default: none)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--ducting", action="store_true",
                    default=os.environ.get("XRADIO_DUCTING", "") not in ("", "0"),
                    help="realism: let the flight's shared weather bring rare tropospheric ducting, "
                         "when VHF reaches far past the horizon over the sea (default: off)")
    ap.add_argument("--dashboard", action="store_true",
                    help="show a live picture of the flight in this terminal instead of a log")
    ap.add_argument("--log", metavar="FILE",
                    help="with --dashboard, write the log to this file as well")
    args = ap.parse_args()

    recent = None
    if args.dashboard:
        # The terminal belongs to the dashboard: the log goes to its "recent"
        # list and, if asked, to a file, never to the screen.
        LOG.setLevel(logging.DEBUG if args.verbose else logging.INFO)
        LOG.propagate = False
        recent = RecentEvents()
        LOG.addHandler(recent)
        if args.log:
            fh = logging.FileHandler(args.log)
            fh.setFormatter(logging.Formatter("%(asctime)s %(levelname)-7s %(message)s"))
            LOG.addHandler(fh)
    else:
        logging.basicConfig(
            level=logging.DEBUG if args.verbose else logging.INFO,
            format="%(asctime)s %(levelname)-7s %(message)s",
        )

    loop = asyncio.get_running_loop()
    server = XRadioServer(password=args.password, ducting=args.ducting)
    if args.password:
        LOG.info("a flight password is required to join")
    if args.ducting:
        LOG.info("tropospheric ducting is on: warm, still weather from the flight's sky can carry VHF far over the sea")
    transport, _ = await loop.create_datagram_endpoint(
        lambda: server, local_addr=(args.host, args.port))
    disable_udp_reset(transport)
    try:
        if recent is not None:
            await asyncio.gather(server.run_traffic_loop(),
                                 Dashboard(server, args.port, recent).run())
        else:
            await server.run_traffic_loop()
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
