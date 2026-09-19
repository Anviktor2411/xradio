#include "server.h"

#include "mathconst.h"
#include "net.h"
#include "protocol.h"

#include <atomic>
#include <algorithm>
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

    double altFt() const { return (double)altM * 3.28084; }

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
    const double rNm = 3440.065;
    const double p1 = a.lat * kPi / 180.0, p2 = b.lat * kPi / 180.0;
    const double dp = p2 - p1;
    const double dl = (b.lon - a.lon) * kPi / 180.0;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * rNm * std::asin(std::min(1.0, std::sqrt(h)));
}

// VHF line-of-sight horizon, 1.23 * (sqrt(h1_ft) + sqrt(h2_ft)), with a floor
// so two aircraft on the same apron can still talk.
double radioRangeNm(const Session& a, const Session& b) {
    const double los = 1.23 * (std::sqrt(std::max(0.0, a.altFt())) +
                               std::sqrt(std::max(0.0, b.altFt())));
    return std::max(15.0, los);
}

bool inRadioRange(const Session& a, const Session& b) {
    return distanceNm(a, b) <= radioRangeNm(a, b);
}

bool finite(float v) { return std::isfinite(v); }

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

    Core(Sink sink, void* ctx) : sink_(sink), ctx_(ctx) {}

    void onPacket(const Peer& from, const uint8_t* data, int len, double now);
    void tick(double now);

    int  clientCount() const { return (int)sessions_.size(); }
    std::vector<std::string> callsigns() const {
        std::vector<std::string> out;
        for (const auto& kv : sessions_) out.push_back(kv.second.callsign);
        std::sort(out.begin(), out.end());
        return out;
    }
    uint64_t sent() const { return sent_; }

private:
    void send(const Peer& to, uint8_t type, uint32_t sid,
              const void* payload = nullptr, int payloadLen = 0);

    void onLogin(const Peer& from, const uint8_t* p, int len, double now);
    void onPosition(Session& s, const uint8_t* p, int len);
    void onText(Session& s, const uint8_t* p, int len);
    void onVoice(Session& s, const uint8_t* p, int len, double now);

    std::vector<Session*> listeners(const Session& sender, uint32_t freqKhz);
    void reap(double now);
    void broadcastTraffic(double now);
    void sendTraffic(Session& me, const std::vector<Session*>& others, double now);
    void drop(const Peer& peer);

    Sink     sink_;
    void*    ctx_;
    std::map<Peer, Session> sessions_;
    uint32_t nextSid_ = 1;
    double   lastTraffic_ = 0;
    uint64_t sent_ = 0;
    double   t0_ = 0;
};

void Core::send(const Peer& to, uint8_t type, uint32_t sid,
                const void* payload, int payloadLen) {
    uint8_t buf[kMaxPacket];
    if (payloadLen < 0 || (size_t)payloadLen + sizeof(Header) > sizeof(buf)) return;

    Header h{};
    h.magic      = kMagic;
    h.type       = type;
    h.version    = (uint8_t)kProtoVersion;
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
    if (h.version != kProtoVersion) return;

    // The declared length must match what actually arrived; a lying header is
    // the first thing a fuzzer tries.
    const uint8_t* payload = data + sizeof(Header);
    const int avail = len - (int)sizeof(Header);
    if ((int)h.payloadLen != avail) return;

    if (h.type == PT_LOGIN) { onLogin(from, payload, avail, now); return; }

    auto it = sessions_.find(from);
    if (it == sessions_.end()) return;      // everything else needs a login first
    Session& s = it->second;
    // The session id only ever goes to the address that logged in, so
    // requiring it here means an off-path spoofer has to guess it first.
    if (h.sessionId != s.sid) return;
    s.lastSeen = now;

    switch (h.type) {
        case PT_POSITION: onPosition(s, payload, avail); break;
        case PT_TEXT:     onText(s, payload, avail); break;
        case PT_VOICE:    onVoice(s, payload, avail, now); break;
        case PT_PING:     send(from, PT_PONG, s.sid); break;
        case PT_LOGOUT:   drop(from); break;
        default: break;
    }
}

void Core::onLogin(const Peer& from, const uint8_t* p, int len, double now) {
    if (len < (int)sizeof(LoginPayload)) return;
    LoginPayload lp{};
    memcpy(&lp, p, sizeof(lp));
    if (lp.protoVer != kProtoVersion) return;

    std::string callsign = clean(lp.callsign, sizeof(lp.callsign), 15);
    std::string acIcao   = clean(lp.acIcao,   sizeof(lp.acIcao),   7);

    sessions_.erase(from);                  // a re-login replaces the old session

    Session s;
    s.sid      = nextSid_++;
    s.peer     = from;
    s.callsign = callsign.empty() ? ("UNK" + std::to_string(s.sid)) : callsign;
    s.acIcao   = acIcao.empty() ? "ZZZZ" : acIcao;
    s.lastSeen = now;
    const uint32_t sid = s.sid;
    sessions_[from] = s;

    LoginAckPayload ack{};
    ack.sessionId    = sid;
    ack.serverTimeMs = (uint32_t)((now - t0_) * 1000.0);
    send(from, PT_LOGIN_ACK, sid, &ack, (int)sizeof(ack));
}

void Core::onPosition(Session& s, const uint8_t* p, int len) {
    if (len < (int)sizeof(PositionPayload)) return;
    PositionPayload pp{};
    memcpy(&pp, p, sizeof(pp));

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

    s.lat = pp.lat; s.lon = pp.lon; s.altM = pp.altMslM;
    s.heading = wrap360(pp.headingTrue);
    s.pitch = pp.pitch; s.roll = pp.roll;
    s.gsMs = pp.gsMs;
    s.gear = std::min(1.f, std::max(0.f, pp.gearRatio));
    s.flap = std::min(1.f, std::max(0.f, pp.flapRatio));
    s.com1 = pp.com1Khz; s.com2 = pp.com2Khz;
    s.lights = pp.lights; s.onGround = pp.onGround;
    s.txRadio = pp.txRadio; s.rxMask = pp.rxMask;
    s.timeMs = pp.timeMs;
    s.track = wrap360(pp.trackTrue);
    s.vsMs = pp.vsMs;
    s.hasPosition = true;
}

void Core::onText(Session& s, const uint8_t* p, int len) {
    if (len < (int)sizeof(TextHeader)) return;
    TextHeader th{};
    memcpy(&th, p, sizeof(th));

    int textLen = (int)th.textLen;
    const int avail = len - (int)sizeof(TextHeader);
    textLen = std::min(textLen, kMaxTextBytes);
    textLen = std::min(textLen, avail);
    if (textLen <= 0) return;

    // Text does not need the PTT held, so it goes out on whichever radio the
    // client names -- but only if that radio is really tuned there, so nobody
    // can transmit on a frequency they are not on.
    uint32_t freq = 0;
    if (th.freqKhz && (th.freqKhz == s.com1 || th.freqKhz == s.com2)) {
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

void Core::onVoice(Session& s, const uint8_t* p, int len, double now) {
    if (len < (int)sizeof(VoiceHeader)) return;
    VoiceHeader vh{};
    memcpy(&vh, p, sizeof(vh));

    int opusLen = (int)vh.opusLen;
    const int avail = len - (int)sizeof(VoiceHeader);
    opusLen = std::min(opusLen, kMaxVoiceBytes);
    opusLen = std::min(opusLen, avail);
    if (opusLen <= 0) return;

    // Same rule as text. Relying on txRadio from the last position report
    // would drop the first ~200 ms of every transmission.
    uint32_t freq = 0;
    if (vh.freqKhz && (vh.freqKhz == s.com1 || vh.freqKhz == s.com2)) {
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
    for (auto& kv : sessions_) {
        Session& peer = kv.second;
        if (peer.sid == sender.sid || !peer.listeningOn(freqKhz)) continue;
        if (sender.hasPosition && peer.hasPosition && !inRadioRange(sender, peer)) {
            continue;
        }
        out.push_back(&peer);
    }
    return out;
}

void Core::drop(const Peer& peer) { sessions_.erase(peer); }

void Core::reap(double now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second.lastSeen > kSessionTimeoutS) it = sessions_.erase(it);
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
}

void Core::broadcastTraffic(double now) {
    std::vector<Session*> live;
    for (auto& kv : sessions_) {
        if (kv.second.hasPosition) live.push_back(&kv.second);
    }
    for (Session* me : live) {
        std::vector<Session*> others;
        for (Session* other : live) {
            if (other->sid == me->sid) continue;
            if (distanceNm(*me, *other) > kTrafficRangeNm) continue;
            others.push_back(other);
        }
        // nearest first, so the packet cap drops the far ones
        std::sort(others.begin(), others.end(), [me](Session* a, Session* b) {
            return distanceNm(*me, *a) < distanceNm(*me, *b);
        });
        if ((int)others.size() > kMaxEntriesPerPacket) {
            others.resize((size_t)kMaxEntriesPerPacket);
        }
        sendTraffic(*me, others, now);
    }
}

void Core::sendTraffic(Session& me, const std::vector<Session*>& others, double now) {
    uint8_t payload[kMaxPacket];
    TrafficHeader th{};
    th.count = (uint16_t)others.size();
    memcpy(payload, &th, sizeof(th));
    size_t off = sizeof(th);

    for (Session* o : others) {
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
        e.txActive = txActive; e.reserved = 0;
        e.timeMs = o->timeMs; e.trackTrue = o->track; e.vsMs = o->vsMs;

        if (off + sizeof(e) > sizeof(payload)) break;
        memcpy(payload + off, &e, sizeof(e));
        off += sizeof(e);
    }
    send(me.peer, PT_TRAFFIC, me.sid, payload, (int)off);
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

double nowSeconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void sinkFn(void*, const Peer& to, const void* data, int len) {
    g_sock.sendTo(to, data, len);
}

void serverThread() {
    Core core(sinkFn, nullptr);
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

bool start(uint16_t port, std::string* err) {
    stop();
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

Status status() {
    std::lock_guard<std::mutex> lk(g_statusMx);
    return g_status;
}

}  // namespace relay
}  // namespace xr
