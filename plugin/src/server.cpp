// The relay server that "Host a flight here" runs inside the plugin. The
// protocol logic is Core, which has no socket at all: it is handed each
// datagram and a clock, and sends through a callback, so the tests can drive
// it. Below Core is the thin part that owns the UDP socket and the thread.
//
// server/server.py is the same thing in Python. Both have to behave
// identically -- tools/test_server_parity.py checks -- so a change to one
// is a change to both.
#include "server.h"

#include "mathconst.h"
#include "net.h"
#include "protocol.h"
#include "radio.h"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xr {
namespace relay {

// ---------------------------------------------------------------------------
// one logged-in pilot
// ---------------------------------------------------------------------------
struct Session {
    uint32_t    sid = 0;
    Peer        peer;
    std::string callsign;
    std::string acIcao;
    std::string livery;
    bool        weatherSource = false;   // claimed the flight's weather and time
    double      lastSeen = 0;

    double  lat = 0, lon = 0;
    float   altM = 0, heading = 0, pitch = 0, roll = 0, gsMs = 0;
    float   gear = 0, flap = 0;
    uint32_t com1 = 0, com2 = 0;
    uint8_t lights = 0, onGround = 1;
    uint8_t txRadio = TX_NONE, rxMask = RX_COM1;
    double  lastVoice = -1e9;
    bool    hasPosition = false;
    uint32_t timeMs = 0;
    float   track = 0, vsMs = 0;
    uint16_t squawk = 1200;          // the four digits as they read on the panel
    uint8_t  xpdrMode = XPDR_ALT;
    uint8_t  xpdrIdent = 0;
    // v6
    bool     atc = false;            // an ATC station: never traffic, listed in PT_ATC_LIST
    bool     injector = false;       // drives aircraft of its own (PT_AI_TRAFFIC)
    bool     ai = false;             // this record is one of those aircraft, not a client
    uint32_t owner = 0;              // ...and this is the injector it belongs to
    std::vector<uint8_t> fp;         // a pilot's latest flight plan, as it will be relayed
    std::map<std::string, char> atisSeen;   // icao -> letter already sent to this pilot
    uint8_t  kind = ATC_OTHER;       // a station's facility, from its callsign
    uint16_t proto = kProtoVersion;  // what the client speaks: a v5 one gets v5 packets
    // v6: the radio model (docs/radio.md)
    float    aglM = 0.f;             // height above the terrain as reported (a station: its antenna)
    bool     aglKnown = false;       // the report carried it; a v5-sized one does not
    uint16_t svcRangeNm = 0;         // a station's service volume as it asked, 0 = by facility
    uint16_t svcCeilingFt = 0;

    double altFt() const { return (double)altM * 3.28084; }
    // The height above the ground the radio counts for this aircraft.
    double radioAglM() const {
        return radio::effectiveAglM(aglKnown, (double)aglM, onGround != 0, (double)altM);
    }

    bool listeningOn(uint32_t freqKhz) const {
        if (freqKhz == 0) return false;
        if ((rxMask & RX_COM1) && com1 == freqKhz) return true;
        if ((rxMask & RX_COM2) && com2 == freqKhz) return true;
        return false;
    }

    uint32_t txFreq() const {
        if (txRadio == TX_COM1) return com1;
        if (txRadio == TX_COM2) return com2;
        return 0;
    }
};

namespace {

// Printable ASCII only: these strings end up on other pilots' screens.
std::string clean(const char* raw, size_t width, size_t maxLen) {
    std::string out;
    for (size_t i = 0; i < width && raw[i]; ++i) {
        const unsigned char c = (unsigned char)raw[i];
        if (c >= 32 && c < 127) out += (char)c;
        if (out.size() >= maxLen) break;
    }
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = out.find_last_not_of(" \t\r\n");
    return out.substr(a, b - a + 1);
}

void padInto(char* dst, size_t size, const std::string& s) {
    memset(dst, 0, size);
    const size_t n = s.size() < size - 1 ? s.size() : size - 1;
    memcpy(dst, s.data(), n);
}

double distanceNm(const Session& a, const Session& b) {
    return radio::distanceNm(a.lat, a.lon, b.lat, b.lon);
}

// A station's antenna height (m), service radius (nm) and ceiling (ft): what
// it sent, or its facility's defaults. server.py's station_radio().
radio::StationDefaults stationRadio(const Session& s) {
    radio::StationDefaults r = radio::stationDefaults(s.kind);
    if (s.aglKnown && s.aglM > 0.f) r.antennaM = (double)s.aglM;
    if (s.svcRangeNm > 0) r.rangeNm = (double)s.svcRangeNm;
    if (s.svcCeilingFt > 0) r.ceilingFt = (double)s.svcCeilingFt;
    return r;
}

// The radio model's view of a session (radio.h; server.py's node_of()).
radio::Node nodeOf(const Session& s) {
    radio::Node n;
    n.lat = s.lat; n.lon = s.lon; n.altMslM = (double)s.altM;
    if (s.atc) {
        const radio::StationDefaults st = stationRadio(s);
        n.heightFt = st.antennaM * radio::kFtPerM;
        n.atc = true; n.rangeNm = st.rangeNm; n.ceilingFt = st.ceilingFt;
    } else {
        n.heightFt = radio::aircraftHeightFt(s.radioAglM());
    }
    return n;
}

// Whether `rx` receives what `tx` sends on `freqKhz`: the model's quality
// above nothing -- inside the horizon worked out from both heights above the
// ground, or inside a station's service volume.
bool inRadioRange(const Session& tx, const Session& rx, uint32_t freqKhz, bool ducting) {
    return radio::link(nodeOf(tx), nodeOf(rx), freqKhz, ducting).quality > 0.0;
}

bool finite(float v) { return std::isfinite(v); }

// v6. EETN_TWR -> EETN: the airport is the head before the first underscore,
// four alphanumerics or nothing. The same rule as protocol.atc_icao().
std::string atcIcao(const std::string& callsign) {
    const size_t us = callsign.find('_');
    std::string head = us == std::string::npos ? callsign : callsign.substr(0, us);
    for (char& c : head) c = (char)toupper((unsigned char)c);
    if (head.size() != 4) return "";
    for (char c : head) if (!isalnum((unsigned char)c)) return "";
    return head;
}

// EETN_TWR -> ATC_TWR: the position is the suffix after the last underscore.
uint8_t atcKind(const std::string& callsign) {
    const size_t us = callsign.rfind('_');
    if (us == std::string::npos) return ATC_OTHER;
    std::string tail = callsign.substr(us + 1);
    for (char& c : tail) c = (char)toupper((unsigned char)c);
    if (tail == "DEL")  return ATC_DEL;
    if (tail == "GND")  return ATC_GND;
    if (tail == "TWR")  return ATC_TWR;
    if (tail == "APP" || tail == "DEP") return ATC_APP;
    if (tail == "CTR")  return ATC_CTR;
    if (tail == "ATIS") return ATC_ATIS;
    return ATC_OTHER;
}

// Python's str.upper() on the ASCII an ATIS icao is allowed to hold.
std::string upperAscii(std::string v) {
    for (char& c : v) c = (char)toupper((unsigned char)c);
    return v;
}

// Python's `x % 360.0`: the result always carries the sign of the divisor,
// unlike C's fmod. 0 - 10 must come out as 350, not -10.
float wrap360(float v) {
    if (!std::isfinite(v)) return 0.f;
    float r = std::fmod(v, 360.f);
    if (r < 0.f) r += 360.f;
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Core -- the server with the socket taken out, so tests can drive it
// ---------------------------------------------------------------------------
class Core {
public:
    // Called for every datagram the server wants to send.
    using Sink = void (*)(void* ctx, const Peer& to, const void* data, int len);

    // `ducting`: the realism switch (docs/radio.md) -- the flight's weather may
    // then bring tropospheric ducting. Off unless whoever runs the server asks.
    Core(Sink sink, void* ctx, std::string password, bool ducting = false)
        : sink_(sink), ctx_(ctx), password_(std::move(password)), ductingEnabled_(ducting) {}

    void onPacket(const Peer& from, const uint8_t* data, int len, double now);
    void tick(double now);

    int  clientCount() const { return (int)sessions_.size(); }
    // The realism switch, while running. Off takes the duct away at once.
    void setDuctingEnabled(bool on) {
        if (!on) setDucting(false);
        ductingEnabled_ = on;
    }
    std::vector<std::string> callsigns() const {
        std::vector<std::string> out;
        for (const auto& kv : sessions_) out.push_back(kv.second.callsign);
        std::sort(out.begin(), out.end());
        return out;
    }
    uint64_t sent() const { return sent_; }

private:
    // `version` 0: the header version the receiver speaks (a v5 client drops
    // a packet with any other in it).
    void send(const Peer& to, uint8_t type, uint32_t sid,
              const void* payload = nullptr, int payloadLen = 0, uint8_t version = 0);

    void onLogin(const Peer& from, const uint8_t* p, int len, double now, uint8_t version);
    void onPosition(Session& s, const uint8_t* p, int len);
    void onText(Session& s, const uint8_t* p, int len);
    void onWeather(Session& s, const uint8_t* p, int len);
    void onVoice(Session& s, const uint8_t* p, int len, double now);
    // v6
    void onAiTraffic(Session& s, const uint8_t* p, int len, double now);
    void onFlightPlan(Session& s, const uint8_t* p, int len);
    void onAtis(Session& s, const uint8_t* p, int len);
    void offerAtis(Session& s);
    void broadcastAtcList(double now);
    Session* speaker(Session& s, uint32_t fromSid);
    Session* bySid(uint32_t sid);

    std::vector<Session*> listeners(const Session& sender, uint32_t freqKhz);
    void reap(double now);
    void broadcastTraffic(double now);
    void sendTraffic(Session& me, const std::vector<Session*>& others, double now);
    void broadcastRoster(double now);
    void drop(const Peer& peer);
    void forget(const Session& s);      // what goes with a session: its aircraft, its ATIS
    void setDucting(bool on);

    Sink     sink_;
    void*    ctx_;
    std::string password_;              // empty: open to all
    bool     ductingEnabled_ = false;   // the realism switch
    bool     ducting_ = false;          // ...and the flight's air, ducting right now
    std::map<Peer, Session> sessions_;
    uint32_t nextSid_ = 1;
    // v6. The aircraft injectors drive, by flight-wide id; the ATIS on the air,
    // by airport; and whether the ATC list changed since it last went out.
    std::map<uint32_t, Session> ai_;
    struct Atis {
        uint32_t freq = 0;
        char     letter = 'A';
        std::string text;
        uint32_t owner = 0;
        std::vector<uint8_t> payload;
    };
    std::map<std::string, Atis> atis_;
    bool     atcDirty_ = false;
    double   lastAtc_ = 0;
    // Whose sky everyone else flies in. The first pilot to claim it keeps it
    // until they leave; two sources would mean the weather flickering between
    // two sims.
    uint32_t weatherSid_ = 0;
    double   lastTraffic_ = 0;
    // Somebody joined or left since the roster last went out.
    bool     rosterDirty_ = false;
    double   lastRoster_ = 0;
    uint16_t rosterEpoch_ = 0;
    uint64_t sent_ = 0;
    double   t0_ = 0;
};

void Core::send(const Peer& to, uint8_t type, uint32_t sid,
                const void* payload, int payloadLen, uint8_t version) {
    uint8_t buf[kMaxPacket];
    if (payloadLen < 0 || (size_t)payloadLen + sizeof(Header) > sizeof(buf)) return;
    if (version == 0) {
        auto it = sessions_.find(to);
        version = (uint8_t)(it != sessions_.end() ? it->second.proto : kProtoVersion);
    }

    Header h{};
    h.magic      = kMagic;
    h.type       = type;
    h.version    = version;
    h.payloadLen = (uint16_t)payloadLen;
    h.sessionId  = sid;
    memcpy(buf, &h, sizeof(h));
    if (payloadLen > 0) memcpy(buf + sizeof(h), payload, (size_t)payloadLen);

    ++sent_;
    sink_(ctx_, to, buf, (int)sizeof(h) + payloadLen);
}

void Core::onPacket(const Peer& from, const uint8_t* data, int len, double now) {
    if (t0_ == 0) t0_ = now;
    if (len < (int)sizeof(Header)) return;

    Header h{};
    memcpy(&h, data, sizeof(h));
    if (h.magic != kMagic) return;
    if (h.version < kMinProtoVersion || h.version > kProtoVersion) {
        // A login from another version is told why it gets nowhere, in its
        // own header version -- an older client drops any other.
        if (h.type == PT_LOGIN) {
            LoginRejectPayload rj{};
            rj.reason = RJ_VERSION;
            send(from, PT_LOGIN_REJECT, 0, &rj, (int)sizeof(rj), h.version);
        }
        return;
    }

    // The declared length must match what actually arrived; a lying header is
    // the first thing a fuzzer tries.
    const uint8_t* payload = data + sizeof(Header);
    const int avail = len - (int)sizeof(Header);
    if ((int)h.payloadLen != avail) return;

    if (h.type == PT_LOGIN) { onLogin(from, payload, avail, now, h.version); return; }

    auto it = sessions_.find(from);
    if (it == sessions_.end()) return;      // everything else needs a login first
    Session& s = it->second;
    // The session id only ever goes to the address that logged in, so
    // requiring it here means an off-path spoofer has to guess it first. And a
    // session speaks the one version it logged in with.
    if (h.sessionId != s.sid || h.version != s.proto) return;
    s.lastSeen = now;

    switch (h.type) {
        case PT_POSITION: onPosition(s, payload, avail); break;
        case PT_TEXT:     onText(s, payload, avail); break;
        case PT_VOICE:    onVoice(s, payload, avail, now); break;
        case PT_PING:     send(from, PT_PONG, s.sid); break;
        case PT_WEATHER:  onWeather(s, payload, avail); break;
        case PT_AI_TRAFFIC: onAiTraffic(s, payload, avail, now); break;
        case PT_FLIGHTPLAN: onFlightPlan(s, payload, avail); break;
        case PT_ATIS:     onAtis(s, payload, avail); break;
        case PT_LOGOUT:   drop(from); break;
        default: break;
    }
}

Session* Core::bySid(uint32_t sid) {
    for (auto& kv : sessions_) if (kv.second.sid == sid) return &kv.second;
    return nullptr;
}

// Who a text or voice packet is from: the session itself, or -- for an
// injector naming one of its own aircraft -- that aircraft.
Session* Core::speaker(Session& s, uint32_t fromSid) {
    if (s.injector && isAiSid(fromSid)) {
        auto it = ai_.find(fromSid);
        if (it != ai_.end() && it->second.owner == s.sid) return &it->second;
    }
    return &s;
}

// The aircraft an injector drives, as TrafficEntry records with the
// injector's own local numbers in sessionId. Validated exactly like a
// position report: the renderer at the other end trusts nothing less.
void Core::onAiTraffic(Session& s, const uint8_t* p, int len, double now) {
    if (!s.injector || len < (int)sizeof(TrafficHeader)) return;
    TrafficHeader th{};
    memcpy(&th, p, sizeof(th));
    int off = (int)sizeof(th);
    int mine = 0;
    for (const auto& kv : ai_) if (kv.second.owner == s.sid) ++mine;
    // v6 entries carry the aircraft's height above the ground at their end;
    // an injector that predates it sends them 108 bytes long.
    const bool v6 = len - off >= (int)th.count * (int)sizeof(TrafficEntry);
    const int esize = v6 ? (int)sizeof(TrafficEntry) : kTrafficEntrySizeV5;
    for (int i = 0; i < th.count; ++i) {
        if (off + esize > len) break;
        TrafficEntry e{};
        memcpy(&e, p + off, (size_t)esize);
        off += esize;
        const uint16_t local = (uint16_t)(e.sessionId & 0xFFFFu);
        if (local == 0) continue;
        const uint32_t sid = aiSid(s.sid, local);
        if (e.reserved & AI_REMOVE) {
            if (ai_.erase(sid)) --mine;
            continue;
        }
        if (!std::isfinite(e.lat) || e.lat < -90.0  || e.lat > 90.0)  continue;
        if (!std::isfinite(e.lon) || e.lon < -180.0 || e.lon > 180.0) continue;
        if (!finite(e.altMslM) || e.altMslM < -1000.f || e.altMslM > 40000.f) continue;
        if (!finite(e.headingTrue) || !finite(e.pitch) || !finite(e.roll) ||
            !finite(e.gsMs) || !finite(e.gearRatio) || !finite(e.flapRatio) ||
            !finite(e.trackTrue) || !finite(e.vsMs)) continue;
        if (e.vsMs < -200.f || e.vsMs > 200.f) continue;
        if (e.gsMs < 0.f || e.gsMs > 1500.f) continue;
        const float aglM = v6 ? e.aglM : 0.f;
        if (!finite(aglM) || aglM < -1000.f || aglM > 40000.f) continue;

        auto it = ai_.find(sid);
        if (it == ai_.end()) {
            if (mine >= kMaxAiPerInjector) continue;
            Session a;
            a.sid = sid; a.ai = true; a.owner = s.sid;
            a.txRadio = TX_NONE; a.rxMask = 0;
            it = ai_.emplace(sid, a).first;
            ++mine;
        }
        Session& a = it->second;
        a.callsign = clean(e.callsign, sizeof(e.callsign), 15);
        if (a.callsign.empty()) a.callsign = "AI" + std::to_string(local);
        a.acIcao = clean(e.acIcao, sizeof(e.acIcao), 7);
        if (a.acIcao.empty()) a.acIcao = "ZZZZ";
        a.lat = e.lat; a.lon = e.lon; a.altM = e.altMslM;
        a.heading = wrap360(e.headingTrue); a.pitch = e.pitch; a.roll = e.roll; a.gsMs = e.gsMs;
        a.gear = std::min(1.f, std::max(0.f, e.gearRatio));
        a.flap = std::min(1.f, std::max(0.f, e.flapRatio));
        a.lights = e.lights; a.onGround = e.onGround;
        a.xpdrMode = e.xpdrMode <= XPDR_TA_RA ? e.xpdrMode : (uint8_t)XPDR_OFF;
        a.timeMs = e.timeMs; a.track = wrap360(e.trackTrue); a.vsMs = e.vsMs;
        a.livery = clean(e.livery, sizeof(e.livery), 15);
        a.squawk = validSquawk(e.squawk) ? e.squawk : 0;
        a.xpdrIdent = e.xpdrIdent ? 1 : 0;
        a.aglM = aglM; a.aglKnown = v6;
        a.hasPosition = true;
        a.lastSeen = now;
    }
}

// A pilot files; every station hears. A station answers; the pilot and the
// other stations hear. The latest copy is kept with the pilot's session so a
// station that joins later still sees it.
void Core::onFlightPlan(Session& s, const uint8_t* p, int len) {
    if (len < (int)sizeof(FlightPlanPayload)) return;
    FlightPlanPayload fp{};
    memcpy(&fp, p, sizeof(fp));
    if (s.atc) {
        Session* target = bySid(fp.sessionId);
        if (!target || target->atc || target->ai) return;
        fp.sessionId = target->sid;
        padInto(fp.callsign, sizeof(fp.callsign), target->callsign);
        target->fp.assign((const uint8_t*)&fp, (const uint8_t*)&fp + sizeof(fp));
        if (target->proto >= 6)                  // a v5 client has no flight plans
            send(target->peer, PT_FLIGHTPLAN, target->sid, &fp, (int)sizeof(fp));
        for (auto& kv : sessions_) {
            if (kv.second.atc && kv.second.sid != s.sid)
                send(kv.first, PT_FLIGHTPLAN, kv.second.sid, &fp, (int)sizeof(fp));
        }
        return;
    }
    if (s.injector) return;              // computer-flown aircraft do not file
    fp.sessionId = s.sid;
    padInto(fp.callsign, sizeof(fp.callsign), s.callsign);   // a pilot files as who they logged in as
    fp.squawk = 0;                                            // only ATC assigns a squawk
    if (fp.status != FP_FILED && fp.status != FP_CANCELLED) fp.status = FP_FILED;
    if (fp.status == FP_CANCELLED) s.fp.clear();
    else s.fp.assign((const uint8_t*)&fp, (const uint8_t*)&fp + sizeof(fp));
    for (auto& kv : sessions_) {
        if (kv.second.atc) send(kv.first, PT_FLIGHTPLAN, kv.second.sid, &fp, (int)sizeof(fp));
    }
}

// A station publishes the ATIS of an airport; it is offered to everyone
// tuned to its frequency, now and when they tune in later.
void Core::onAtis(Session& s, const uint8_t* p, int len) {
    if (!s.atc || len < (int)sizeof(AtisPayload)) return;
    AtisPayload ah{};
    memcpy(&ah, p, sizeof(ah));
    const std::string icao = upperAscii(clean(ah.icao, sizeof(ah.icao), 7));
    int textLen = std::min<int>(ah.textLen, kMaxAtisBytes);
    textLen = std::min(textLen, len - (int)sizeof(ah));
    if (textLen < 0) textLen = 0;
    if (icao.empty() || ah.freqKhz == 0) return;
    char letter = ah.letter;
    if (letter < 'A' || letter > 'Z') letter = 'A';
    const std::string text((const char*)p + sizeof(ah), (size_t)textLen);

    AtisPayload oh{};
    oh.freqKhz = ah.freqKhz;
    padInto(oh.icao, sizeof(oh.icao), icao);
    oh.letter = letter;
    oh.textLen = (uint16_t)textLen;
    std::vector<uint8_t> payload((const uint8_t*)&oh, (const uint8_t*)&oh + sizeof(oh));
    payload.insert(payload.end(), text.begin(), text.end());

    auto old = atis_.find(icao);
    const bool fresh = old == atis_.end();
    const bool freqChanged = fresh || old->second.freq != ah.freqKhz;
    const bool changed = fresh || old->second.letter != letter || old->second.text != text;
    Atis& a = atis_[icao];
    a.freq = ah.freqKhz; a.letter = letter; a.text = text; a.owner = s.sid; a.payload = payload;
    if (freqChanged) atcDirty_ = true;
    if (changed) {
        for (auto& kv : sessions_) {
            kv.second.atisSeen.erase(icao);        // the new letter goes out again
            offerAtis(kv.second);
        }
    }
}

// Send a pilot the ATIS of every frequency they have just tuned.
void Core::offerAtis(Session& s) {
    if (s.ai || s.injector || s.proto < 6) return;
    for (auto& kv : atis_) {
        const Atis& a = kv.second;
        if (a.owner == s.sid) continue;
        if (s.com1 != a.freq && s.com2 != a.freq) continue;
        auto seen = s.atisSeen.find(kv.first);
        if (seen != s.atisSeen.end() && seen->second == a.letter) continue;
        s.atisSeen[kv.first] = a.letter;
        send(s.peer, PT_ATIS, s.sid, a.payload.data(), (int)a.payload.size());
    }
}

// The stations online and the ATIS frequencies, to everyone -- and whether
// the flight's air is ducting tonight. Ordered by session so both servers
// produce the same bytes. A v5 client has no such list.
void Core::broadcastAtcList(double now) {
    lastAtc_ = now;
    atcDirty_ = false;
    if (sessions_.empty()) return;

    std::vector<const Session*> stations;
    for (const auto& kv : sessions_) if (kv.second.atc) stations.push_back(&kv.second);
    std::sort(stations.begin(), stations.end(),
              [](const Session* a, const Session* b) { return a->sid < b->sid; });

    uint8_t payload[kMaxPacket];
    size_t off = sizeof(AtcListHeader);
    int count = 0;
    auto put = [&](const AtcEntry& e) {
        if (count >= kMaxAtcEntries) return;
        memcpy(payload + off, &e, sizeof(e));
        off += sizeof(e);
        ++count;
    };
    // Where it transmits from and what it covers: what the server routes by,
    // so a pilot's radio can work out the same signal (docs/radio.md).
    auto radioInto = [](AtcEntry& e, const Session* where, double antennaM, double rangeNm, double ceilingFt) {
        const bool has = where && where->hasPosition;
        e.flags = has ? AE_POSITION : 0;
        e.lat = has ? where->lat : 0.0;
        e.lon = has ? where->lon : 0.0;
        e.altMslM = has ? where->altM : 0.f;
        e.antennaM = (float)antennaM;
        e.rangeNm = (uint16_t)std::min(65535.0, rangeNm);
        e.ceilingFt = (uint16_t)std::min(65535.0, ceilingFt);
    };
    for (const Session* s : stations) {
        AtcEntry e{};
        e.sessionId = s->sid;
        padInto(e.callsign, sizeof(e.callsign), s->callsign);
        padInto(e.icao, sizeof(e.icao), atcIcao(s->callsign));
        e.freqKhz = s->com1;
        e.kind = s->kind;
        const radio::StationDefaults st = stationRadio(*s);
        radioInto(e, s, st.antennaM, st.rangeNm, st.ceilingFt);
        put(e);
    }
    for (const auto& kv : atis_) {                 // std::map: sorted by icao, like Python's sorted()
        AtcEntry e{};
        e.sessionId = 0;
        padInto(e.callsign, sizeof(e.callsign), kv.first + "_ATIS");
        padInto(e.icao, sizeof(e.icao), kv.first);
        e.freqKhz = kv.second.freq;
        e.kind = ATC_ATIS;
        const Session* owner = bySid(kv.second.owner);
        const radio::StationDefaults atis = radio::stationDefaults(ATC_ATIS);
        radioInto(e, owner, owner ? stationRadio(*owner).antennaM : atis.antennaM, atis.rangeNm, atis.ceilingFt);
        put(e);
    }
    AtcListHeader lh{};
    lh.count = (uint16_t)count;
    lh.flags = ducting_ ? AL_DUCTING : 0;
    memcpy(payload, &lh, sizeof(lh));
    for (const auto& kv : sessions_) {
        if (kv.second.proto >= 6) send(kv.first, PT_ATC_LIST, kv.second.sid, payload, (int)off);
    }
}

void Core::onLogin(const Peer& from, const uint8_t* p, int len, double now, uint8_t version) {
    if (len < (int)sizeof(LoginPayload)) return;
    LoginPayload lp{};
    memcpy(&lp, p, sizeof(lp));
    if (lp.protoVer != version || lp.protoVer < kMinProtoVersion || lp.protoVer > kProtoVersion) {
        LoginRejectPayload rj{};
        rj.reason = RJ_VERSION;
        send(from, PT_LOGIN_REJECT, 0, &rj, (int)sizeof(rj), version);
        return;
    }
    if (!password_.empty()) {
        // fixed-width, may lack a terminator
        std::string given(lp.password, strnlen(lp.password, sizeof(lp.password)));
        if (given != password_) {
            LoginRejectPayload rj{};
            rj.reason = RJ_PASSWORD;
            send(from, PT_LOGIN_REJECT, 0, &rj, (int)sizeof(rj), version);
            return;
        }
    }
    if (lp.protoVer < 6) lp.flags &= LF_WEATHER_SOURCE;   // the station and injector bits are v6's

    std::string callsign = clean(lp.callsign, sizeof(lp.callsign), 15);
    std::string acIcao   = clean(lp.acIcao,   sizeof(lp.acIcao),   7);
    std::string livery   = clean(lp.livery,   sizeof(lp.livery),   15);

    // A re-login from the same address replaces the old session, and
    // everything that pointed at the old one lets go of it -- the weather
    // claim above all. It used to survive here, naming a session that no
    // longer existed, so a host whose login ack was lost, or who logged in
    // again after a network stall, could never share the sky again and nor
    // could anybody else. drop() also marks the roster for resending.
    drop(from);

    Session s;
    s.sid      = nextSid_++;
    s.peer     = from;
    s.callsign = callsign.empty() ? ("UNK" + std::to_string(s.sid)) : callsign;
    s.acIcao   = acIcao.empty() ? "ZZZZ" : acIcao;
    s.livery   = livery;
    s.weatherSource = (lp.flags & LF_WEATHER_SOURCE) != 0;
    s.atc      = (lp.flags & LF_ATC) != 0;
    s.injector = (lp.flags & LF_INJECTOR) != 0;
    s.proto    = lp.protoVer;
    s.kind     = s.atc ? atcKind(s.callsign) : (uint8_t)ATC_OTHER;
    s.lastSeen = now;
    const uint32_t sid = s.sid;
    sessions_[from] = s;
    if (s.weatherSource && weatherSid_ == 0) weatherSid_ = sid;

    LoginAckPayload ack{};
    ack.sessionId    = sid;
    ack.serverTimeMs = (uint32_t)((now - t0_) * 1000.0);
    send(from, PT_LOGIN_ACK, sid, &ack, (int)sizeof(ack));
    rosterDirty_ = true;
    atcDirty_ = true;
    // A station that joins late still gets every plan in the flight.
    if (s.atc) {
        for (const auto& kv : sessions_) {
            const Session& p = kv.second;
            if (!p.fp.empty() && p.sid != sid)
                send(from, PT_FLIGHTPLAN, sid, p.fp.data(), (int)p.fp.size());
        }
    }
}

void Core::onPosition(Session& s, const uint8_t* p, int len) {
    // v6 adds the radio model's fields at the end; a v5-sized report is still
    // a position, its height above the ground then unknown.
    if (len < kPositionSizeV5) return;
    const bool v6 = len >= (int)sizeof(PositionPayload);
    PositionPayload pp{};
    memcpy(&pp, p, v6 ? sizeof(pp) : (size_t)kPositionSizeV5);

    // A client can send anything. NaN or a wild coordinate would be relayed to
    // everyone else and poison their renderer, and it breaks the distance
    // maths here too, so a bad report is dropped at the door.
    if (!std::isfinite(pp.lat) || pp.lat < -90.0  || pp.lat > 90.0)  return;
    if (!std::isfinite(pp.lon) || pp.lon < -180.0 || pp.lon > 180.0) return;
    if (!finite(pp.altMslM) || pp.altMslM < -1000.f || pp.altMslM > 40000.f) return;
    if (!finite(pp.headingTrue) || !finite(pp.pitch) || !finite(pp.roll) ||
        !finite(pp.gsMs) || !finite(pp.gearRatio) || !finite(pp.flapRatio) ||
        !finite(pp.trackTrue) || !finite(pp.vsMs)) return;
    if (pp.vsMs < -200.f || pp.vsMs > 200.f) return;
    if (pp.gsMs < 0.f || pp.gsMs > 1500.f) return;
    if (!finite(pp.aglM) || pp.aglM < -1000.f || pp.aglM > 40000.f) return;

    // the list says where a station is and what it covers
    if (s.atc && (!s.hasPosition || s.aglM != pp.aglM || s.aglKnown != v6 ||
                  s.svcRangeNm != pp.svcRangeNm || s.svcCeilingFt != pp.svcCeilingFt))
        atcDirty_ = true;
    s.lat = pp.lat; s.lon = pp.lon; s.altM = pp.altMslM;
    s.aglM = pp.aglM; s.aglKnown = v6;
    s.svcRangeNm = pp.svcRangeNm; s.svcCeilingFt = pp.svcCeilingFt;
    s.heading = wrap360(pp.headingTrue);
    s.pitch = pp.pitch; s.roll = pp.roll;
    s.gsMs = pp.gsMs;
    s.gear = std::min(1.f, std::max(0.f, pp.gearRatio));
    s.flap = std::min(1.f, std::max(0.f, pp.flapRatio));
    if (s.atc && s.com1 != pp.com1Khz) atcDirty_ = true;   // a station retuned: the list says its frequency
    s.com1 = pp.com1Khz; s.com2 = pp.com2Khz;
    s.lights = pp.lights; s.onGround = pp.onGround;
    s.txRadio = pp.txRadio; s.rxMask = pp.rxMask;
    s.timeMs = pp.timeMs;
    s.track = wrap360(pp.trackTrue);
    s.vsMs = pp.vsMs;
    // A squawk is four octal digits. Anything else is a client bug or a
    // client being clever, and either way it is not relayed as if real.
    s.squawk = validSquawk(pp.squawk) ? pp.squawk : 0;
    s.xpdrMode = pp.xpdrMode <= XPDR_TA_RA ? pp.xpdrMode : (uint8_t)XPDR_OFF;
    s.xpdrIdent = pp.xpdrIdent ? 1 : 0;
    s.hasPosition = true;
    offerAtis(s);
}

void Core::onText(Session& client, const uint8_t* p, int len) {
    if (len < (int)sizeof(TextHeader)) return;
    TextHeader th{};
    memcpy(&th, p, sizeof(th));

    int textLen = (int)th.textLen;
    const int avail = len - (int)sizeof(TextHeader);
    textLen = std::min(textLen, kMaxTextBytes);
    textLen = std::min(textLen, avail);
    if (textLen <= 0) return;
    // v6: an injector speaking as one of its aircraft. Such an aircraft has
    // no radios of its own to check against, so the frequency named in the
    // packet is the one it transmits on.
    Session& s = *speaker(client, th.fromSession);

    // A message addressed to a callsign is not a transmission: it goes to
    // that one pilot wherever they are, on no frequency at all, and nobody
    // else sees it. A pilot who cannot be found is told so, rather than
    // being left to wonder whether the message landed.
    const std::string to = clean(th.to, sizeof(th.to), 15);
    if (!to.empty()) {
        Session* target = nullptr;
        for (auto& kv : sessions_) {
            if (kv.second.callsign == to) { target = &kv.second; break; }
        }

        uint8_t dm[kMaxPacket];
        TextHeader dh{};
        dh.freqKhz     = 0;                     // not on the radio
        dh.fromSession = target ? s.sid : 0;
        padInto(dh.from, sizeof(dh.from), target ? s.callsign : std::string("XRADIO"));
        padInto(dh.to, sizeof(dh.to), to);

        if (target) {
            dh.textLen = (uint16_t)textLen;
            memcpy(dm, &dh, sizeof(dh));
            memcpy(dm + sizeof(dh), p + sizeof(TextHeader), (size_t)textLen);
            send(target->peer, PT_TEXT, target->sid, dm, (int)sizeof(dh) + textLen);
        } else {
            const std::string why = "nobody called " + to + " is in this flight";
            dh.textLen = (uint16_t)why.size();
            memcpy(dm, &dh, sizeof(dh));
            memcpy(dm + sizeof(dh), why.data(), why.size());
            send(s.peer, PT_TEXT, s.sid, dm, (int)sizeof(dh) + (int)why.size());
        }
        return;
    }

    // Text does not need the PTT held, so it goes out on whichever radio the
    // client names -- but only if that radio is really tuned there, so nobody
    // can transmit on a frequency they are not on.
    uint32_t freq = 0;
    if (s.ai) {
        freq = th.freqKhz;
    } else if (th.freqKhz && (th.freqKhz == s.com1 || th.freqKhz == s.com2)) {
        freq = th.freqKhz;
    } else if (th.freqKhz == 0) {
        freq = s.txFreq();
        if (freq == 0) freq = s.com1;
    }
    if (freq == 0) return;

    uint8_t out[kMaxPacket];
    TextHeader oh{};
    oh.freqKhz     = freq;
    oh.fromSession = s.sid;
    padInto(oh.from, sizeof(oh.from), s.callsign);
    oh.textLen     = (uint16_t)textLen;
    memcpy(out, &oh, sizeof(oh));
    memcpy(out + sizeof(oh), p + sizeof(TextHeader), (size_t)textLen);
    const int outLen = (int)sizeof(oh) + textLen;

    for (Session* peer : listeners(s, freq)) {
        send(peer->peer, PT_TEXT, peer->sid, out, outLen);
    }
}

void Core::onVoice(Session& client, const uint8_t* p, int len, double now) {
    if (len < (int)sizeof(VoiceHeader)) return;
    VoiceHeader vh{};
    memcpy(&vh, p, sizeof(vh));

    int opusLen = (int)vh.opusLen;
    const int avail = len - (int)sizeof(VoiceHeader);
    opusLen = std::min(opusLen, kMaxVoiceBytes);
    opusLen = std::min(opusLen, avail);
    if (opusLen <= 0) return;
    Session& s = *speaker(client, vh.fromSession);

    // Same rule as text. Relying on txRadio from the last position report
    // would drop the first ~200 ms of every transmission.
    uint32_t freq = 0;
    if (s.ai) {
        freq = vh.freqKhz;
        // so the [TX] flag on the aircraft follows its voice like a pilot's
        s.com1 = freq; s.txRadio = TX_COM1;
    } else if (vh.freqKhz && (vh.freqKhz == s.com1 || vh.freqKhz == s.com2)) {
        freq = vh.freqKhz;
    } else if (vh.freqKhz == 0) {
        freq = s.txFreq();
    }
    if (freq == 0) return;

    s.lastVoice = now;

    uint8_t out[kMaxPacket];
    VoiceHeader oh{};
    oh.freqKhz     = freq;
    oh.fromSession = s.sid;
    oh.seq         = vh.seq;
    oh.opusLen     = (uint16_t)opusLen;
    memcpy(out, &oh, sizeof(oh));
    memcpy(out + sizeof(oh), p + sizeof(VoiceHeader), (size_t)opusLen);
    const int outLen = (int)sizeof(oh) + opusLen;

    for (Session* peer : listeners(s, freq)) {
        send(peer->peer, PT_VOICE, peer->sid, out, outLen);
    }
}

std::vector<Session*> Core::listeners(const Session& sender, uint32_t freqKhz) {
    std::vector<Session*> out;
    // Guard reaches everyone in range whether or not they have it tuned. It is
    // the one frequency you can call somebody on without already knowing where
    // they are listening, which is the whole reason it exists.
    const bool guard = isGuard(freqKhz);
    for (auto& kv : sessions_) {
        Session& peer = kv.second;
        if (peer.sid == sender.sid) continue;
        if (sender.ai && peer.sid == sender.owner) continue;   // the program driving it said it; it knows
        if (!guard && !peer.listeningOn(freqKhz)) continue;
        if (sender.hasPosition && peer.hasPosition && !inRadioRange(sender, peer, freqKhz, ducting_)) {
            continue;
        }
        out.push_back(&peer);
    }
    return out;
}

// One sim decides the sky; the rest are told about it. Relayed untouched and
// only from the session that claimed it at login, so a joining pilot cannot
// quietly move everyone else's weather.
void Core::onWeather(Session& s, const uint8_t* p, int len) {
    if (len != (int)sizeof(WeatherPayload)) return;
    if (weatherSid_ == 0 && s.weatherSource) weatherSid_ = s.sid;  // host rejoined
    if (s.sid != weatherSid_) return;
    for (auto& kv : sessions_) {
        if (kv.second.sid == s.sid) continue;
        send(kv.first, PT_WEATHER, kv.second.sid, p, len);
    }
    if (ductingEnabled_) {
        WeatherPayload w{};
        memcpy(&w, p, sizeof(w));
        setDucting(radio::ductingFrom(w));
    }
}

// The flight's air, ducting or not; the ATC list carries it to every radio at once.
void Core::setDucting(bool on) {
    if (on == ducting_) return;
    ducting_ = on;
    atcDirty_ = true;
}

// v6: what goes with a session when it goes -- its aircraft, its ATIS.
void Core::forget(const Session& s) {
    if (s.injector) {
        for (auto it = ai_.begin(); it != ai_.end();) {
            if (it->second.owner == s.sid) it = ai_.erase(it);
            else ++it;
        }
    }
    if (s.atc) {
        for (auto it = atis_.begin(); it != atis_.end();) {
            if (it->second.owner == s.sid) it = atis_.erase(it);
            else ++it;
        }
        atcDirty_ = true;
    }
}

void Core::drop(const Peer& peer) {
    auto it = sessions_.find(peer);
    if (it == sessions_.end()) return;
    // with no shared sky, nothing says the air is layered
    if (it->second.sid == weatherSid_) { weatherSid_ = 0; setDucting(false); }
    forget(it->second);
    sessions_.erase(it);
    rosterDirty_ = true;
}

void Core::reap(double now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second.lastSeen > kSessionTimeoutS) {
            if (it->second.sid == weatherSid_) { weatherSid_ = 0; setDucting(false); }
            forget(it->second);
            it = sessions_.erase(it);
            rosterDirty_ = true;
        }
        else ++it;
    }
    for (auto it = ai_.begin(); it != ai_.end();) {
        if (now - it->second.lastSeen > kAiTimeoutS) it = ai_.erase(it);
        else ++it;
    }
}

void Core::tick(double now) {
    if (t0_ == 0) t0_ = now;
    const double period = 1.0 / kTrafficHz;
    if (now - lastTraffic_ < period) return;
    lastTraffic_ = now;
    reap(now);
    broadcastTraffic(now);
    if (rosterDirty_ || now - lastRoster_ >= kRosterIntervalS) broadcastRoster(now);
    if (atcDirty_ || now - lastAtc_ >= kAtcListIntervalS) broadcastAtcList(now);
}

// Everyone in the flight, to everyone in the flight -- the receiver included,
// which it leaves out of its own list. Ordered by session so both servers
// produce the same bytes, and split like traffic when it does not fit.
void Core::broadcastRoster(double now) {
    lastRoster_  = now;
    rosterDirty_ = false;
    if (sessions_.empty()) return;

    // An injector is a program, not a pilot anyone would message; its
    // aircraft are not in the roster either (they cannot read one).
    std::vector<const Session*> all;
    for (const auto& kv : sessions_) if (!kv.second.injector) all.push_back(&kv.second);
    std::sort(all.begin(), all.end(),
              [](const Session* a, const Session* b) { return a->sid < b->sid; });
    if ((int)all.size() > kMaxEntriesTotal) all.resize((size_t)kMaxEntriesTotal);

    rosterEpoch_ = (uint16_t)(rosterEpoch_ + 1);
    const size_t per   = (size_t)kMaxRosterEntries;
    const size_t parts = (all.size() + per - 1) / per;

    for (const auto& kv : sessions_) {
        const Session& to = kv.second;
        for (size_t part = 0; part < parts; ++part) {
            const size_t first = part * per;
            const size_t n = std::min(per, all.size() - first);
            uint8_t payload[kMaxPacket];
            RosterHeader rh{};
            rh.epoch = rosterEpoch_;
            rh.part  = (uint8_t)part;
            rh.parts = (uint8_t)parts;
            rh.count = (uint16_t)n;
            memcpy(payload, &rh, sizeof(rh));
            size_t off = sizeof(rh);
            for (size_t i = first; i < first + n; ++i) {
                RosterEntry e{};
                e.sessionId = all[i]->sid;
                padInto(e.callsign, sizeof(e.callsign), all[i]->callsign);
                padInto(e.acIcao,   sizeof(e.acIcao),   all[i]->acIcao);
                memcpy(payload + off, &e, sizeof(e));
                off += sizeof(e);
            }
            send(to.peer, PT_ROSTER, to.sid, payload, (int)off);
        }
    }
}

void Core::broadcastTraffic(double now) {
    std::vector<Session*> live;
    for (auto& kv : sessions_) {
        if (kv.second.hasPosition) live.push_back(&kv.second);
    }
    // What can be seen: pilots, and the aircraft injectors drive. An ATC
    // station is not an aircraft, and an injector does not need its own
    // aircraft back.
    std::vector<Session*> visible;
    for (Session* s : live) if (!s->atc) visible.push_back(s);
    for (auto& kv : ai_) visible.push_back(&kv.second);
    for (Session* me : live) {
        std::vector<Session*> others;
        for (Session* other : visible) {
            if (other->sid == me->sid || (other->ai && other->owner == me->sid)) continue;
            if (distanceNm(*me, *other) > kTrafficRangeNm) continue;
            others.push_back(other);
        }
        // nearest first, so the packet cap drops the far ones
        std::sort(others.begin(), others.end(), [me](Session* a, Session* b) {
            return distanceNm(*me, *a) < distanceNm(*me, *b);
        });
        if ((int)others.size() > kMaxEntriesTotal) {
            others.resize((size_t)kMaxEntriesTotal);
        }
        sendTraffic(*me, others, now);
    }
}

// Sent in as many packets as it takes. The count in each header is the number
// of entries in that packet, decided before anything is written -- a header
// that promises more than it carries is how a receiver ends up reading a
// neighbouring aircraft's bytes as a position.
void Core::sendTraffic(Session& me, const std::vector<Session*>& others, double now) {
  // A v6 receiver gets each aircraft's height above the ground as well -- the
  // one its radio is routed by, so its own model agrees; a v5 one gets the
  // entries it knows, which are shorter.
  const bool v6 = me.proto >= 6;
  const size_t esize = v6 ? sizeof(TrafficEntry) : (size_t)kTrafficEntrySizeV5;
  const size_t per = (size_t)(v6 ? kMaxEntriesPerPacket : kMaxTrafficEntriesV5);
  for (size_t first = 0; first == 0 || first < others.size(); first += per) {
    const size_t n = std::min(per, others.size() - first);
    uint8_t payload[kMaxPacket];
    TrafficHeader th{};
    th.count = (uint16_t)n;
    memcpy(payload, &th, sizeof(th));
    size_t off = sizeof(th);

    for (size_t i = first; i < first + n; ++i) {
        Session* o = others[i];
        uint8_t txActive = 0;
        if (now - o->lastVoice < kTxHoldS) {
            const uint32_t f = o->txFreq();
            txActive = (f && me.listeningOn(f)) ? 1 : 0;
        }
        TrafficEntry e{};
        e.sessionId = o->sid;
        padInto(e.callsign, sizeof(e.callsign), o->callsign);
        padInto(e.acIcao,   sizeof(e.acIcao),   o->acIcao);
        e.lat = o->lat; e.lon = o->lon;
        e.altMslM = o->altM; e.headingTrue = o->heading;
        e.pitch = o->pitch; e.roll = o->roll;
        e.gsMs = o->gsMs; e.gearRatio = o->gear; e.flapRatio = o->flap;
        e.lights = o->lights; e.onGround = o->onGround;
        e.txActive = txActive; e.xpdrMode = o->xpdrMode;
        e.timeMs = o->timeMs; e.trackTrue = o->track; e.vsMs = o->vsMs;
        padInto(e.livery, sizeof(e.livery), o->livery);
        e.squawk = o->squawk; e.xpdrIdent = o->xpdrIdent; e.reserved = 0;
        e.aglM = (float)o->radioAglM();

        if (off + esize > sizeof(payload)) break;
        memcpy(payload + off, &e, esize);
        off += esize;
    }
    send(me.peer, PT_TRAFFIC, me.sid, payload, (int)off);
  }
}

// ---------------------------------------------------------------------------
// the hosted server: socket + thread
// ---------------------------------------------------------------------------
namespace {

UdpServerSocket   g_sock;
std::thread       g_thread;
std::atomic<bool> g_run{false};
std::mutex        g_statusMx;
Status            g_status;
std::string       g_password;
std::atomic<bool> g_ducting{false};

double nowSeconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void sinkFn(void*, const Peer& to, const void* data, int len) {
    g_sock.sendTo(to, data, len);
}

void serverThread() {
    Core core(sinkFn, nullptr, g_password, g_ducting.load());
    uint8_t buf[kMaxPacket];
    uint64_t in = 0;
    double lastStatus = 0;

    while (g_run.load()) {
        Peer from;
        // 20 ms: short enough that the 10 Hz traffic tick is never late,
        // long enough that an idle server costs nothing.
        const int n = g_sock.recvFrom(buf, (int)sizeof(buf), &from, 20);
        const double now = nowSeconds();
        if (n > 0) {
            ++in;
            core.onPacket(from, buf, n, now);
        }
        core.setDuctingEnabled(g_ducting.load());
        core.tick(now);

        if (now - lastStatus > 0.5) {
            lastStatus = now;
            std::lock_guard<std::mutex> lk(g_statusMx);
            g_status.clients    = core.clientCount();
            g_status.callsigns  = core.callsigns();
            g_status.packetsIn  = in;
            g_status.packetsOut = core.sent();
        }
    }
}

}  // namespace

bool start(uint16_t port, const std::string& password, std::string* err, bool ducting) {
    stop();
    g_password = password;
    g_ducting.store(ducting);
    std::string e;
    if (!g_sock.open(port, "", &e)) {
        if (err) *err = e;
        std::lock_guard<std::mutex> lk(g_statusMx);
        g_status = Status{};
        g_status.error = e;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        g_status = Status{};
        g_status.running = true;
        g_status.port = port;
    }
    g_run.store(true);
    g_thread = std::thread(serverThread);
    return true;
}

void stop() {
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();
    g_sock.close();
    std::lock_guard<std::mutex> lk(g_statusMx);
    const std::string keepErr = g_status.error;
    g_status = Status{};
    g_status.error = keepErr;
}

bool running() { return g_run.load(); }

void setDucting(bool on) { g_ducting.store(on); }

Status status() {
    std::lock_guard<std::mutex> lk(g_statusMx);
    return g_status;
}

}  // namespace relay
}  // namespace xr
