"""XRadio wire protocol -- Python side.

Mirrors plugin/src/protocol.h exactly. Little-endian, byte-packed ('<' prefix
means no alignment padding, which matches #pragma pack(1) on the C++ side).

v6 adds air traffic control: ATC stations (LF_ATC), one session that drives
many computer-flown aircraft (LF_INJECTOR + PT_AI_TRAFFIC), flight plans
(PT_FLIGHTPLAN), a published ATIS (PT_ATIS) and the list of stations online
(PT_ATC_LIST) -- and the radio model's fields: heights above the ground, the
stations' antennas and service volumes (docs/radio.md). Read protocol.h for the
reasoning behind each.
"""

import struct

MAGIC = 0x31435258  # b"XRC1" little-endian
PROTO_VERSION = 6   # v6: ATC stations, injected AI traffic, flight plans, ATIS, the radio model
# The oldest client a server still lets in: a v5 pilot (XRadio 0.5) flies in a
# v6 flight on what v5 knew -- v5 headers, v5-sized records, no v6-only packets.
MIN_PROTO_VERSION = 5

# packet types
PT_LOGIN = 1
PT_LOGIN_ACK = 2
PT_POSITION = 3
PT_TRAFFIC = 4
PT_TEXT = 5
PT_VOICE = 6
PT_PING = 7
PT_PONG = 8
PT_LOGOUT = 9
PT_LOGIN_REJECT = 10
PT_WEATHER = 11
PT_ROSTER = 12     # server -> client: everyone in the flight; see protocol.h
# v6: air traffic control -- see protocol.h for the whole story
PT_AI_TRAFFIC = 13   # injector -> server: the aircraft it drives (TRAFFIC_HDR + TRAFFIC_ENTRY...)
PT_FLIGHTPLAN = 14   # pilot -> server -> ATC stations; ATC -> server -> pilot
PT_ATIS = 15         # ATC -> server (publish); server -> pilot who tunes it
PT_ATC_LIST = 16     # server -> everyone: the stations online and the ATIS frequencies
PT_AIRCRAFT = 17     # pilot -> server (echoed back as the receipt) -> ATC stations: the loaded aircraft
PT_TCAS = 18         # pilot -> server -> ATC stations: the pilot's own TCAS advisory

# AIRCRAFT.source
AS_DEFAULT, AS_DETECTED, AS_MAPPED, AS_MANUAL = 0, 1, 2, 3
# TCAS.level
TCAS_NONE, TCAS_TA, TCAS_RA, TCAS_CLEAR = 0, 1, 2, 3

# The international emergency frequency, and in XRadio the one place everybody
# can be reached: a transmission on it goes to every pilot within VHF range
# whatever they have tuned. That is what guard is for -- you do not know what
# frequency the other aircraft is on. The horizon still applies; it is a radio.
GUARD_KHZ = 121500


def is_guard(freq_khz: int) -> bool:
    return freq_khz == GUARD_KHZ

RJ_PASSWORD, RJ_VERSION = 1, 2

LF_WEATHER_SOURCE = 1 << 0    # LoginPayload.flags: "my weather is the flight's"
LF_ATC = 1 << 1               # v6: an ATC station -- never traffic, listed in PT_ATC_LIST, gets flight plans
LF_INJECTOR = 1 << 2          # v6: may send PT_AI_TRAFFIC and speak as one of its aircraft

# v6. The flight-wide session id of an injected aircraft: the injector's
# session in the high half, its local number in the low half, and the top bit
# so it can never collide with a real session.
AI_SID_BIT = 0x80000000


def ai_sid(injector_sid: int, local_id: int) -> int:
    return AI_SID_BIT | ((injector_sid & 0x7FFF) << 16) | (local_id & 0xFFFF)


def is_ai_sid(sid: int) -> bool:
    return bool(sid & AI_SID_BIT)


def ai_owner(sid: int) -> int:
    return (sid >> 16) & 0x7FFF


AI_REMOVE = 1 << 0            # TRAFFIC_ENTRY.reserved in PT_AI_TRAFFIC: drop this aircraft now

AL_DUCTING = 1 << 0           # ATC_LIST_HDR.flags: VHF carries far over the sea tonight (docs/radio.md)
AE_POSITION = 1 << 0          # ATC_ENTRY.flags: lat, lon and altitude are real

# FlightPlanPayload.rules / .status
FP_IFR, FP_VFR, FP_Y, FP_Z = ord("I"), ord("V"), ord("Y"), ord("Z")
FP_FILED, FP_ACK, FP_CLEARED, FP_CANCELLED = 1, 2, 3, 4

# AtcEntry.kind
ATC_OTHER, ATC_DEL, ATC_GND, ATC_TWR, ATC_APP, ATC_CTR, ATC_ATIS = 0, 1, 2, 3, 4, 5, 6
ATC_KINDS = {"DEL": ATC_DEL, "GND": ATC_GND, "TWR": ATC_TWR, "APP": ATC_APP, "CTR": ATC_CTR,
             "DEP": ATC_APP, "ATIS": ATC_ATIS}


def atc_kind(callsign: str) -> int:
    """EETN_TWR -> ATC_TWR; the position is the suffix after the underscore."""
    suffix = callsign.rsplit("_", 1)[-1].upper() if "_" in callsign else ""
    return ATC_KINDS.get(suffix, ATC_OTHER)


def atc_icao(callsign: str) -> str:
    """EETN_TWR -> EETN; empty when the callsign does not start with an airport."""
    head = callsign.split("_", 1)[0].upper()
    return head if len(head) == 4 and head.isalnum() else ""


MAX_ATIS_BYTES = 600

TX_NONE, TX_COM1, TX_COM2 = 0, 1, 2
RX_COM1, RX_COM2 = 1, 2

# Transponder mode, numbered exactly as X-Plane numbers it in both
# sim/cockpit/radios/transponder_mode and sim/cockpit2/tcas/targets/ssr_mode,
# so a mode read out of one sim goes straight into the other end's TCAS.
XPDR_OFF, XPDR_STANDBY, XPDR_ON, XPDR_ALT = 0, 1, 2, 3
XPDR_TEST, XPDR_GROUND, XPDR_TA_ONLY, XPDR_TA_RA = 4, 5, 6, 7


def xpdr_transmitting(mode: int) -> bool:
    """Above standby is transmitting, so it is on other aircraft's TCAS."""
    return mode > XPDR_STANDBY

# The whole datagram, header included. 1200 rather than the 1400 that fits an
# ethernet MTU, because a group whose router will not forward a port puts
# everyone on Tailscale or another WireGuard-based VPN, and those hand you a
# 1280-byte link -- 1252 once IPv4 and UDP headers are on. See protocol.h.
MAX_PACKET = 1200

# How many aircraft fit in one traffic packet, worked out rather than written
# down. Must equal xr::kMaxTrafficEntries -- and, for a v5 client's shorter
# entries, xr::kMaxTrafficEntriesV5.
MAX_TRAFFIC_ENTRIES = (MAX_PACKET - 12 - 4) // 112
MAX_TRAFFIC_ENTRIES_V5 = (MAX_PACKET - 12 - 4) // 108

# And how many pilots fit in one roster packet. Must equal xr::kMaxRosterEntries.
MAX_ROSTER_ENTRIES = (MAX_PACKET - 12 - 8) // 28
# v6: stations in one ATC list packet. Must equal xr::kMaxAtcEntries.
MAX_ATC_ENTRIES = (MAX_PACKET - 12 - 4) // 64

HEADER = struct.Struct("<IBBHI")          # magic, type, version, payloadLen, sessionId
LOGIN = struct.Struct("<16s8sHH16s32s")   # callsign, acIcao, protoVer, flags, livery, password
LOGIN_ACK = struct.Struct("<II")          # sessionId, serverTimeMs
LOGIN_REJECT = struct.Struct("<HH")       # reason, reserved
# see PositionPayload: ... squawk, xpdrMode, xpdrIdent, then v6 aglM, svcRangeNm, svcCeilingFt
POSITION = struct.Struct("<2d7f2I4BI2fHBBfHH")
POSITION_V5 = struct.Struct("<2d7f2I4BI2fHBB")   # the same without the v6 radio fields (v5 clients)
TRAFFIC_HDR = struct.Struct("<HH")        # count, reserved
TRAFFIC_ENTRY = struct.Struct("<I16s8s2d7f4BI2f16sHBBf")  # ... + livery, squawk, ident, reserved, v6 aglM
TRAFFIC_ENTRY_V5 = struct.Struct("<I16s8s2d7f4BI2f16sHBB")  # without aglM: what a v5 client reads
TEXT_HDR = struct.Struct("<II16sH16s")    # freqKhz, fromSession, from, textLen, to
VOICE_HDR = struct.Struct("<IIHH")        # freqKhz, fromSession, seq, opusLen
ROSTER_HDR = struct.Struct("<HBBHH")      # epoch, part, parts, count, reserved
ROSTER_ENTRY = struct.Struct("<I16s8s")   # sessionId, callsign, acIcao
# v6
FLIGHTPLAN = struct.Struct("<I16s8sBBH8s8s8sI128s64sHH")   # see FlightPlanPayload
ATIS_HDR = struct.Struct("<I8scBH")       # freqKhz, icao, letter, reserved, textLen (+ text)
ATC_LIST_HDR = struct.Struct("<HH")       # count, flags (AL_*)
# sessionId, callsign, icao, freqKhz, kind, flags (AE_*), reserved,
# lat, lon, altMslM, antennaM, rangeNm, ceilingFt -- see AtcEntry
ATC_ENTRY = struct.Struct("<I16s8sIBB2s2d2fHH")
# acIcao, tailnum, airline, livery, wingspanDm, lengthDm, mtowKg, wtc, source, reserved -- see AircraftPayload
AIRCRAFT = struct.Struct("<8s12s4s32sHHIcBH")
# timeMs, level, sense, message, reserved, vsTargetMs, vsMs, intruderSid -- see TcasPayload
TCAS = struct.Struct("<IBbBBffI")

# The flight's shared sky: time, then X-Plane 12's region weather whole --
# three cloud layers and thirteen altitude levels, the sim's own counts.
CLOUD_LAYERS = 3
AIR_LAYERS = 13
WEATHER = struct.Struct("<Ififfffffi" + f"{4 * CLOUD_LAYERS}f" + f"{7 * AIR_LAYERS}f")

# Sanity: these sizes are asserted against the C++ structs in tools/check_sizes.py
assert HEADER.size == 12
assert LOGIN.size == 76
assert POSITION.size == 80 and POSITION_V5.size == 72
assert TRAFFIC_ENTRY.size == 112 and TRAFFIC_ENTRY_V5.size == 108
assert TEXT_HDR.size == 42
assert WEATHER.size == 452
assert FLIGHTPLAN.size == 256
assert ATIS_HDR.size == 16
assert ATC_ENTRY.size == 64
assert AIRCRAFT.size == 68
assert TCAS.size == 20


def pack(ptype: int, session_id: int, payload: bytes = b"", version: int = PROTO_VERSION) -> bytes:
    """Wrap a payload in a protocol header.

    `version` is for a client that can also talk to an older server (the
    header is the same in every version); everything in this file speaks
    PROTO_VERSION unless told otherwise."""
    return HEADER.pack(MAGIC, ptype, version, len(payload), session_id) + payload


def unpack_header(data: bytes):
    """Return (type, version, payload_len, session_id, payload) or None if invalid."""
    if len(data) < HEADER.size:
        return None
    magic, ptype, version, plen, sid = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        return None
    # The declared length must be exactly what arrived -- no slack either
    # way -- and nothing bigger than a packet is ever ours.
    if len(data) != HEADER.size + plen or len(data) > MAX_PACKET:
        return None
    payload = data[HEADER.size:]
    return ptype, version, plen, sid, payload


def login(callsign: str, ac_icao: str = "C172", livery: str = "", password: str = "",
          flags: int = 0, version: int = PROTO_VERSION) -> bytes:
    """A LOGIN payload, the way every client builds it."""
    return LOGIN.pack(pad(callsign, 16), pad(ac_icao, 8), version, flags,
                      pad(livery, 16), pad(password, 32))


def flightplan(callsign: str, ac_icao: str, dep: str, dest: str, route: str = "", rules: str = "V",
               cruise_ft: int = 0, altn: str = "", remarks: str = "", status: int = FP_FILED,
               squawk: int = 0, session_id: int = 0) -> bytes:
    """A FLIGHTPLAN payload. A pilot files with session_id 0; the server fills it in."""
    return FLIGHTPLAN.pack(session_id, pad(callsign, 16), pad(ac_icao, 8), ord(rules[:1] or "V"), status,
                           squawk, pad(dep, 8), pad(dest, 8), pad(altn, 8), int(cruise_ft),
                           pad(route, 128), pad(remarks, 64), 0, 0)


def unpack_flightplan(payload: bytes):
    """The dict form of a FLIGHTPLAN payload (None if too short)."""
    if len(payload) < FLIGHTPLAN.size:
        return None
    (sid, cs, icao, rules, status, squawk, dep, dest, altn, cruise, route, remarks, _f, _r) = \
        FLIGHTPLAN.unpack_from(payload, 0)
    return {"session_id": sid, "callsign": cstr(cs), "ac_icao": cstr(icao),
            "rules": chr(rules) if 32 <= rules < 127 else "V", "status": status, "squawk": squawk,
            "dep": cstr(dep), "dest": cstr(dest), "altn": cstr(altn), "cruise_ft": cruise,
            "route": cstr(route), "remarks": cstr(remarks)}


def atis(freq_khz: int, icao: str, letter: str, text: str) -> bytes:
    """An ATIS payload: header plus the text, capped at MAX_ATIS_BYTES."""
    data = text.encode("utf-8")[:MAX_ATIS_BYTES]
    return ATIS_HDR.pack(freq_khz, pad(icao, 8), (letter[:1] or "A").encode("ascii", "replace"),
                         0, len(data)) + data


def aircraft(ac_icao: str, tailnum: str = "", airline: str = "", livery: str = "", span_m: float = 0.0,
             length_m: float = 0.0, mtow_kg: int = 0, wtc: str = "", source: int = AS_DETECTED) -> bytes:
    """An AIRCRAFT payload: the type the sim has loaded, sent whenever it changes."""
    return AIRCRAFT.pack(pad(ac_icao, 8), pad(tailnum, 12), pad(airline, 4), pad(livery, 32),
                         int(round(span_m * 10)), int(round(length_m * 10)), int(mtow_kg),
                         (wtc[:1] or "\0").encode("ascii", "replace"), source, 0)


def unpack_aircraft(payload: bytes):
    if len(payload) < AIRCRAFT.size:
        return None
    icao, tail, airline, livery, span, length, mtow, wtc, source, _r = AIRCRAFT.unpack_from(payload, 0)
    w = wtc.decode("ascii", "replace").strip("\0")
    return {"ac_icao": cstr(icao), "tailnum": cstr(tail), "airline": cstr(airline), "livery": cstr(livery),
            "span_m": span / 10, "length_m": length / 10, "mtow_kg": mtow, "wtc": w, "source": source}


def tcas(level: int, sense: int = 0, message: int = 0, vs_target_ms: float = 0.0, vs_ms: float = 0.0,
         intruder_sid: int = 0, time_ms: int = 0) -> bytes:
    return TCAS.pack(time_ms & 0xFFFFFFFF, level, sense, message, 0, vs_target_ms, vs_ms, intruder_sid)


def unpack_tcas(payload: bytes):
    if len(payload) < TCAS.size:
        return None
    t, level, sense, message, _r, vst, vs, intr = TCAS.unpack_from(payload, 0)
    return {"time_ms": t, "level": level, "sense": sense, "message": message, "vs_target_ms": vst, "vs_ms": vs,
            "intruder_sid": intr}


def cstr(raw: bytes) -> str:
    """Decode a null-padded fixed-width C string."""
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def pad(s: str, size: int) -> bytes:
    """Encode to a null-padded fixed-width C string."""
    return s.encode("utf-8")[:size - 1].ljust(size, b"\x00")
