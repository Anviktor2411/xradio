// XRadio wire protocol -- shared between the X-Plane plugin (C++) and the
// relay server (Python). Everything is little-endian and byte-packed.
//
// Keep this file and server/protocol.py in sync. If you change a struct here,
// change the matching struct.Struct format string there.
#pragma once

#include <stdint.h>

namespace xr {

// "XRC1" as little-endian bytes.
static const uint32_t kMagic       = 0x31435258u;
static const uint16_t kProtoVersion = 5;   // v5: the roster, and 121.500 as guard

enum PacketType : uint8_t {
    PT_LOGIN     = 1,  // client -> server
    PT_LOGIN_ACK = 2,  // server -> client
    PT_POSITION  = 3,  // client -> server, ~5 Hz
    PT_TRAFFIC   = 4,  // server -> client, ~5 Hz
    PT_TEXT      = 5,  // both ways, radio text message on a frequency
    PT_VOICE     = 6,  // both ways, one Opus frame on a frequency
    PT_PING      = 7,  // client -> server keepalive
    PT_PONG      = 8,  // server -> client
    PT_LOGOUT    = 9,  // client -> server
    PT_LOGIN_REJECT = 10,  // server -> client: why the login was refused
    PT_WEATHER   = 11,  // weather source -> server -> everyone else
};

// The international emergency frequency, and in XRadio the one place everybody
// can be reached. A transmission on it is relayed to every pilot within VHF
// range whatever they have tuned, which is what guard is for: you do not know
// what frequency the other aircraft is on, so you call them on the one
// everybody is supposed to be listening to. Still a radio -- the horizon
// applies exactly as it does on any other frequency.
static const uint32_t kGuardKhz = 121500;
inline bool isGuard(uint32_t freqKhz) { return freqKhz == kGuardKhz; }

// LoginPayload.flags
static const uint16_t LF_WEATHER_SOURCE = 1 << 0;  // "my weather and time are the flight's"

// LoginRejectPayload.reason
enum RejectReason : uint16_t {
    RJ_PASSWORD = 1,   // wrong or missing flight password
    RJ_VERSION  = 2,   // client and server are different XRadio versions
};

// Transmit selector
enum TxRadio : uint8_t { TX_NONE = 0, TX_COM1 = 1, TX_COM2 = 2 };

// Transponder mode. Deliberately the same numbering X-Plane uses for both
// `sim/cockpit/radios/transponder_mode` and the TCAS target dataref
// `sim/cockpit2/tcas/targets/ssr_mode`, and the same as XPMP2's
// XPMPTransponderMode -- so a mode read out of one sim goes into the other
// end's TCAS untranslated, and nobody has to remember a mapping.
enum XpdrMode : uint8_t {
    XPDR_OFF     = 0,
    XPDR_STANDBY = 1,
    XPDR_ON      = 2,   // mode A: identity only, no altitude
    XPDR_ALT     = 3,   // mode C: identity and altitude
    XPDR_TEST    = 4,
    XPDR_GROUND  = 5,   // mode S
    XPDR_TA_ONLY = 6,
    XPDR_TA_RA   = 7,
};

// A squawk is four octal digits, so 7777 is the largest there is and no digit
// may be an 8 or a 9. Stored as the decimal number that spells those digits,
// which is what the panel shows and what X-Plane keeps.
inline bool validSquawk(uint16_t code) {
    if (code > 7777) return false;
    for (uint16_t c = code; c; c /= 10)
        if (c % 10 > 7) return false;
    return true;
}

// Anything above standby is transmitting, so it is on other aircraft's TCAS.
inline bool xpdrTransmitting(uint8_t mode) { return mode > XPDR_STANDBY; }
// Only mode C and above report pressure altitude. A mode A target is a
// bearing-only target: you know it is there, not how high it is.
inline bool xpdrReportsAltitude(uint8_t mode) {
    return mode >= XPDR_ALT && mode != XPDR_TEST;
}

// Receive mask bits
static const uint8_t RX_COM1 = 1 << 0;
static const uint8_t RX_COM2 = 1 << 1;

// Light bits
static const uint8_t LT_NAV     = 1 << 0;
static const uint8_t LT_BEACON  = 1 << 1;
static const uint8_t LT_STROBE  = 1 << 2;
static const uint8_t LT_LANDING = 1 << 3;
static const uint8_t LT_TAXI    = 1 << 4;

#pragma pack(push, 1)

struct Header {              // 12 bytes
    uint32_t magic;
    uint8_t  type;
    uint8_t  version;        // kProtoVersion
    uint16_t payloadLen;     // bytes after this header
    uint32_t sessionId;      // 0 until the server assigns one
};

struct LoginPayload {        // 76 bytes
    char     callsign[16];   // null-padded, e.g. "ESNA12"
    char     acIcao[8];      // ICAO type code, e.g. "C172"
    uint16_t protoVer;
    uint16_t flags;          // LF_* bits; was reserved, so old builds send 0
    // v3
    char     livery[16];     // the aircraft's livery folder name, for CSL matching
    char     password[32];   // the flight password; empty if the server has none
};

struct LoginAckPayload {     // 8 bytes
    uint32_t sessionId;
    uint32_t serverTimeMs;
};

struct LoginRejectPayload {  // 4 bytes
    uint16_t reason;         // RejectReason
    uint16_t reserved;
};

// State of our own aircraft, sent to the server.
struct PositionPayload {     // 72 bytes (v4)
    double   lat;            // degrees
    double   lon;            // degrees
    float    altMslM;        // metres MSL
    float    headingTrue;    // degrees, where the nose points
    float    pitch;          // degrees
    float    roll;           // degrees
    float    gsMs;           // ground speed, m/s
    float    gearRatio;      // 0..1
    float    flapRatio;      // 0..1
    uint32_t com1Khz;        // e.g. 118000 == 118.000 MHz
    uint32_t com2Khz;
    uint8_t  lights;         // LT_* bits
    uint8_t  onGround;
    uint8_t  txRadio;        // TxRadio
    uint8_t  rxMask;         // RX_* bits
    // v2: lets receivers interpolate in the sender's own timeline, so the
    // relay's timing cannot make the aircraft stutter.
    uint32_t timeMs;         // sender's clock, ms, wraps freely
    float    trackTrue;      // degrees, direction of travel (differs from heading in wind)
    float    vsMs;           // vertical speed, m/s, up positive
    // v4. The squawk is the four digits as they read on the panel -- 1200 is
    // literally 1200 -- which is how X-Plane stores it too. Every digit is
    // 0..7, so 7777 is the largest there is.
    uint16_t squawk;
    uint8_t  xpdrMode;       // XpdrMode
    uint8_t  xpdrIdent;      // 1 while IDENT is being squawked
};

// One other aircraft, as the server sees it.
struct TrafficEntry {        // 108 bytes (v4)
    uint32_t sessionId;
    char     callsign[16];
    char     acIcao[8];
    double   lat;
    double   lon;
    float    altMslM;
    float    headingTrue;
    float    pitch;
    float    roll;
    float    gsMs;
    float    gearRatio;
    float    flapRatio;
    uint8_t  lights;
    uint8_t  onGround;
    uint8_t  txActive;       // 1 while this aircraft is keying a radio we hear
    uint8_t  xpdrMode;       // v4, was reserved: XpdrMode
    uint32_t timeMs;         // the sender's timestamp, passed through untouched
    float    trackTrue;
    float    vsMs;
    char     livery[16];     // v3, from the login
    uint16_t squawk;         // v4
    uint8_t  xpdrIdent;      // v4
    uint8_t  reserved;
};

struct TrafficHeader {       // 4 bytes, followed by `count` TrafficEntry
    uint16_t count;
    uint16_t reserved;
};


struct TextHeader {          // 42 bytes (v5), followed by `textLen` UTF-8 bytes
    uint32_t freqKhz;
    uint32_t fromSession;
    char     from[16];
    uint16_t textLen;
    // v5. Empty for an ordinary radio call, which goes to whoever has that
    // frequency tuned and is inside the horizon. With a callsign in it the
    // message is not a transmission at all: it reaches that one pilot
    // wherever they are and whatever they have tuned, and nobody else hears
    // it. Both halves of the window label it as what it is, because a pilot
    // who mistakes one for the other will say something on the wrong one.
    char     to[16];
};

// The flight's shared sky: what the host's sim reports, applied by everyone
// who is following. The layer counts are X-Plane 12's own -- three cloud
// layers and thirteen altitude levels -- so this is the region weather whole,
// not an approximation of it.
static const int kCloudLayers = 3;
static const int kAirLayers   = 13;

struct WeatherPayload {      // 452 bytes
    uint32_t timeMs;         // sender's clock, so a late packet can be ignored
    float    zuluTimeSec;    // sim/time/zulu_time_sec
    int32_t  dateDays;       // sim/time/local_date_days
    float    visibilitySm;
    float    seaLevelPressurePa;
    float    seaLevelTempC;
    float    rainPercent;
    float    snowCover;
    float    thermalRateMs;
    int32_t  changeMode;     // 0..7, how the weather is trending
    float    cloudType[kCloudLayers];
    float    cloudCoverage[kCloudLayers];
    float    cloudBaseM[kCloudLayers];
    float    cloudTopsM[kCloudLayers];
    float    windAltM[kAirLayers];
    float    windSpeedMs[kAirLayers];
    float    windDirDeg[kAirLayers];
    float    turbulence[kAirLayers];
    float    tempAltM[kAirLayers];
    float    tempAloftC[kAirLayers];
    float    dewpointC[kAirLayers];
};

struct VoiceHeader {         // 12 bytes, followed by `opusLen` bytes of Opus
    uint32_t freqKhz;
    uint32_t fromSession;
    uint16_t seq;
    uint16_t opusLen;
};

#pragma pack(pop)

// The whole datagram, header included.
//
// 1200 rather than the 1400 that fits an ethernet MTU, because a fair number
// of XRadio flights do not go over plain ethernet: a group whose router will
// not forward a port puts everyone on Tailscale or another WireGuard-based
// VPN, and those hand you a 1280-byte link. With IPv4 and UDP headers on top
// that leaves 1252 bytes, and a packet over it is fragmented -- which mostly
// works, right up until the path where it does not, and then it looks like
// traffic that stutters or voice that breaks up for one pilot only.
static const int kMaxPacket = 1200;

// How many aircraft fit in one traffic packet. Worked out from the structs
// rather than written down, because it was written down once and went quietly
// wrong the next time an entry grew.
static const int kMaxTrafficEntries =
    (kMaxPacket - (int)sizeof(Header) - (int)sizeof(TrafficHeader)) /
    (int)sizeof(TrafficEntry);

}  // namespace xr
