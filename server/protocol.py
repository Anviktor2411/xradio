"""XRadio wire protocol -- Python side.

Mirrors plugin/src/protocol.h exactly. Little-endian, byte-packed ('<' prefix
means no alignment padding, which matches #pragma pack(1) on the C++ side).
"""

import struct

MAGIC = 0x31435258  # b"XRC1" little-endian
PROTO_VERSION = 3   # v3: livery, flight password, login rejection

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

RJ_PASSWORD, RJ_VERSION = 1, 2

TX_NONE, TX_COM1, TX_COM2 = 0, 1, 2
RX_COM1, RX_COM2 = 1, 2

MAX_PACKET = 1400

HEADER = struct.Struct("<IBBHI")          # magic, type, version, payloadLen, sessionId
LOGIN = struct.Struct("<16s8sHH16s32s")   # callsign, acIcao, protoVer, reserved, livery, password
LOGIN_ACK = struct.Struct("<II")          # sessionId, serverTimeMs
LOGIN_REJECT = struct.Struct("<HH")       # reason, reserved
POSITION = struct.Struct("<2d7f2I4BI2f")  # see PositionPayload
TRAFFIC_HDR = struct.Struct("<HH")        # count, reserved
TRAFFIC_ENTRY = struct.Struct("<I16s8s2d7f4BI2f16s")   # ... + livery
TEXT_HDR = struct.Struct("<II16sH")       # freqKhz, fromSession, from, textLen
VOICE_HDR = struct.Struct("<IIHH")        # freqKhz, fromSession, seq, opusLen

# Sanity: these sizes are asserted against the C++ structs in tools/check_sizes.py
assert HEADER.size == 12
assert LOGIN.size == 76
assert POSITION.size == 68
assert TRAFFIC_ENTRY.size == 104


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
