"""XRadio wire protocol -- Python side.

Mirrors plugin/src/protocol.h exactly. Little-endian, byte-packed ('<' prefix
means no alignment padding, which matches #pragma pack(1) on the C++ side).
"""

import struct

MAGIC = 0x31435258  # b"XRC1" little-endian
PROTO_VERSION = 5   # v5: the roster, and 121.500 as guard

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

# The international emergency frequency, and in XRadio the one place everybody
# can be reached: a transmission on it goes to every pilot within VHF range
# whatever they have tuned. That is what guard is for -- you do not know what
# frequency the other aircraft is on. The horizon still applies; it is a radio.
GUARD_KHZ = 121500


def is_guard(freq_khz: int) -> bool:
    return freq_khz == GUARD_KHZ

RJ_PASSWORD, RJ_VERSION = 1, 2

LF_WEATHER_SOURCE = 1 << 0    # LoginPayload.flags: "my weather is the flight's"

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
# down. Must equal xr::kMaxTrafficEntries.
MAX_TRAFFIC_ENTRIES = (MAX_PACKET - 12 - 4) // 108

HEADER = struct.Struct("<IBBHI")          # magic, type, version, payloadLen, sessionId
LOGIN = struct.Struct("<16s8sHH16s32s")   # callsign, acIcao, protoVer, flags, livery, password
LOGIN_ACK = struct.Struct("<II")          # sessionId, serverTimeMs
LOGIN_REJECT = struct.Struct("<HH")       # reason, reserved
POSITION = struct.Struct("<2d7f2I4BI2fHBB")  # see PositionPayload
TRAFFIC_HDR = struct.Struct("<HH")        # count, reserved
TRAFFIC_ENTRY = struct.Struct("<I16s8s2d7f4BI2f16sHBB")  # ... + livery, squawk
TEXT_HDR = struct.Struct("<II16sH16s")    # freqKhz, fromSession, from, textLen, to
VOICE_HDR = struct.Struct("<IIHH")        # freqKhz, fromSession, seq, opusLen

# The flight's shared sky: time, then X-Plane 12's region weather whole --
# three cloud layers and thirteen altitude levels, the sim's own counts.
CLOUD_LAYERS = 3
AIR_LAYERS = 13
WEATHER = struct.Struct("<Ififfffffi" + f"{4 * CLOUD_LAYERS}f" + f"{7 * AIR_LAYERS}f")

# Sanity: these sizes are asserted against the C++ structs in tools/check_sizes.py
assert HEADER.size == 12
assert LOGIN.size == 76
assert POSITION.size == 72
assert TRAFFIC_ENTRY.size == 108
assert TEXT_HDR.size == 42
assert WEATHER.size == 452


def pack(ptype: int, session_id: int, payload: bytes = b"") -> bytes:
    """Wrap a payload in a protocol header."""
    return HEADER.pack(MAGIC, ptype, PROTO_VERSION, len(payload), session_id) + payload


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


def login(callsign: str, ac_icao: str = "C172", livery: str = "", password: str = "") -> bytes:
    """A LOGIN payload, the way every client builds it."""
    return LOGIN.pack(pad(callsign, 16), pad(ac_icao, 8), PROTO_VERSION, 0,
                      pad(livery, 16), pad(password, 32))


def cstr(raw: bytes) -> str:
    """Decode a null-padded fixed-width C string."""
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def pad(s: str, size: int) -> bytes:
    """Encode to a null-padded fixed-width C string."""
    return s.encode("utf-8")[:size - 1].ljust(size, b"\x00")
