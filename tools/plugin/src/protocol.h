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
static const uint16_t kProtoVersion = 2;   // v2: timestamped positions, track, vertical speed

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
};

// Transmit selector
enum TxRadio : uint8_t { TX_NONE = 0, TX_COM1 = 1, TX_COM2 = 2 };

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

struct LoginPayload {        // 28 bytes
    char     callsign[16];   // null-padded, e.g. "ESNA12"
    char     acIcao[8];      // ICAO type code, e.g. "C172"
    uint16_t protoVer;
    uint16_t reserved;
};

struct LoginAckPayload {     // 8 bytes
    uint32_t sessionId;
    uint32_t serverTimeMs;
};

// State of our own aircraft, sent to the server.
struct PositionPayload {     // 68 bytes
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
};

// One other aircraft, as the server sees it.
struct TrafficEntry {        // 88 bytes
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
    uint8_t  reserved;
    uint32_t timeMs;         // the sender's timestamp, passed through untouched
    float    trackTrue;
    float    vsMs;
};

struct TrafficHeader {       // 4 bytes, followed by `count` TrafficEntry
    uint16_t count;
    uint16_t reserved;
};

struct TextHeader {          // 26 bytes, followed by `textLen` UTF-8 bytes
    uint32_t freqKhz;
    uint32_t fromSession;
    char     from[16];
    uint16_t textLen;
};

struct VoiceHeader {         // 12 bytes, followed by `opusLen` bytes of Opus
    uint32_t freqKhz;
    uint32_t fromSession;
    uint16_t seq;
    uint16_t opusLen;
};

#pragma pack(pop)

static const int kMaxPacket = 1400;   // stay under a typical MTU

}  // namespace xr
