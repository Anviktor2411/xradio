// XRadio -- X-Plane 12 multiplayer + radio plugin.
//
// Main thread (flight loop, 5 Hz): sends our position, drains the inbox of
// non-voice packets, drives XPMP2 and the windows.
// Network thread: receives everything; voice frames go straight to the mixer
// (voice.cpp), everything else is queued for the main thread. Also sends the
// frames the microphone produces.

#define _USE_MATH_DEFINES   // MSVC needs this before <cmath> for M_PI

#include "net.h"
#include "protocol.h"
#include "voice.h"
#include "xpmp_bridge.h"

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(XPLM300) || !defined(XPLM400)
#  error "Build with -DXPLM200 -DXPLM210 -DXPLM300 -DXPLM301 -DXPLM400 for X-Plane 12"
#endif

namespace {

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------
struct Config {
    std::string host     = "127.0.0.1";
    int         port     = 49100;
    std::string callsign = "XRADIO1";
    std::string acIcao   = "C172";
};

Config      g_cfg;
std::string g_cfgPath;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void logMsg(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[600];
    snprintf(line, sizeof(line), "XRadio: %s\n", buf);
    XPLMDebugString(line);
}

// Folder that holds this plugin, i.e. .../Resources/plugins/XRadio.
// XPLMGetPluginInfo gives us .../XRadio/<platform>_x64/<platform>.xpl, so we
// strip two levels off it.
std::string pluginRootDir() {
    char path[1024] = {0};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
    XPLMExtractFileAndPath(path);   // -> .../XRadio/<platform>_x64
    XPLMExtractFileAndPath(path);   // -> .../XRadio
    return std::string(path);
}

// <X-Plane>/Output/preferences/xradio.cfg
void resolveConfigPath() {
    char prefs[1024] = {0};
    XPLMGetPrefsPath(prefs);
    XPLMExtractFileAndPath(prefs);          // truncates to the directory
    g_cfgPath = std::string(prefs) + XPLMGetDirectorySeparator() + "xradio.cfg";
}

// Writes g_cfg to disk. Used for the first-run defaults and by Settings.
bool saveConfig() {
    FILE* f = fopen(g_cfgPath.c_str(), "w");
    if (!f) {
        logMsg("cannot write %s", g_cfgPath.c_str());
        return false;
    }
    fprintf(f,
            "# XRadio configuration\n"
            "host = %s\n"
            "port = %d\n"
            "callsign = %s\n"
            "actype = %s\n",
            g_cfg.host.c_str(), g_cfg.port, g_cfg.callsign.c_str(), g_cfg.acIcao.c_str());
    fclose(f);
    logMsg("saved config to %s", g_cfgPath.c_str());
    return true;
}

void loadConfig() {
    resolveConfigPath();
    FILE* f = fopen(g_cfgPath.c_str(), "r");
    if (!f) { saveConfig(); return; }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(s.substr(0, eq));
        std::string v = trim(s.substr(eq + 1));
        if      (k == "host")     g_cfg.host = v;
        else if (k == "port")     g_cfg.port = atoi(v.c_str());
        else if (k == "callsign") g_cfg.callsign = v;
        else if (k == "actype")   g_cfg.acIcao = v;
    }
    fclose(f);
    logMsg("config: %s:%d as %s (%s)", g_cfg.host.c_str(), g_cfg.port,
           g_cfg.callsign.c_str(), g_cfg.acIcao.c_str());
}

// ---------------------------------------------------------------------------
// datarefs
// ---------------------------------------------------------------------------
struct Refs {
    XPLMDataRef lat, lon, elev, psi, theta, phi, gs;
    XPLMDataRef gear, flap, onGround;
    XPLMDataRef com1, com2, audioComSel, rxCom1, rxCom2;
    XPLMDataRef ltNav, ltBeacon, ltStrobe, ltLanding, ltTaxi;
} g_ref;

int g_missingRefs = 0;

// Resolve a dataref and say so in Log.txt if X-Plane does not know the name.
// Reading a null ref is harmless (the getters below return 0), but silently
// sending zeros for every flap position is the kind of bug that takes an
// evening to find, so make it loud.
XPLMDataRef findRef(const char* name) {
    XPLMDataRef r = XPLMFindDataRef(name);
    if (!r) {
        ++g_missingRefs;
        logMsg("WARNING: dataref not found: %s", name);
    }
    return r;
}

void findRefs() {
    g_ref.lat      = findRef("sim/flightmodel/position/latitude");
    g_ref.lon      = findRef("sim/flightmodel/position/longitude");
    g_ref.elev     = findRef("sim/flightmodel/position/elevation");
    g_ref.psi      = findRef("sim/flightmodel/position/psi");
    g_ref.theta    = findRef("sim/flightmodel/position/theta");
    g_ref.phi      = findRef("sim/flightmodel/position/phi");
    g_ref.gs       = findRef("sim/flightmodel/position/groundspeed");
    g_ref.gear     = findRef("sim/flightmodel2/gear/deploy_ratio");
    g_ref.flap     = findRef("sim/cockpit2/controls/flap_ratio");
    g_ref.onGround = findRef("sim/flightmodel/failures/onground_any");

    // 8.33 kHz variant reports the frequency in kHz, e.g. 118000 == 118.000 MHz
    g_ref.com1        = findRef("sim/cockpit2/radios/actuators/com1_frequency_hz_833");
    g_ref.com2        = findRef("sim/cockpit2/radios/actuators/com2_frequency_hz_833");
    g_ref.audioComSel = findRef("sim/cockpit2/radios/actuators/audio_com_selection");
    g_ref.rxCom1      = findRef("sim/cockpit2/radios/actuators/audio_selection_com1");
    g_ref.rxCom2      = findRef("sim/cockpit2/radios/actuators/audio_selection_com2");

    g_ref.ltNav     = findRef("sim/cockpit2/switches/navigation_lights_on");
    g_ref.ltBeacon  = findRef("sim/cockpit2/switches/beacon_on");
    g_ref.ltStrobe  = findRef("sim/cockpit2/switches/strobe_lights_on");
    g_ref.ltLanding = findRef("sim/cockpit2/switches/landing_lights_on");
    g_ref.ltTaxi    = findRef("sim/cockpit2/switches/taxi_light_on");

    if (g_missingRefs) {
        logMsg("%d dataref(s) missing -- those values will be sent as zero",
               g_missingRefs);
    }
}

float  fd(XPLMDataRef r) { return r ? XPLMGetDataf(r) : 0.f; }
double dd(XPLMDataRef r) { return r ? XPLMGetDatad(r) : 0.0; }
int    id(XPLMDataRef r) { return r ? XPLMGetDatai(r) : 0; }

float firstOfArray(XPLMDataRef r) {
    if (!r) return 0.f;
    float v = 0.f;
    if (XPLMGetDatavf(r, &v, 0, 1) < 1) return 0.f;
    return v;
}

// ---------------------------------------------------------------------------
// remote aircraft
// ---------------------------------------------------------------------------
struct Remote {
    uint32_t    sid = 0;
    std::string callsign, acIcao;
    double      lat = 0, lon = 0;
    float       altMslM = 0, heading = 0, pitch = 0, roll = 0, gsMs = 0;
    float       gear = 0, flap = 0;
    uint8_t     lights = 0, onGround = 0, txActive = 0;
    float       lastSeen = 0;   // seconds since plugin start
};

std::map<uint32_t, Remote> g_remote;
int g_rejected = 0;   // traffic entries dropped as implausible

// ---------------------------------------------------------------------------
// plugin state
// ---------------------------------------------------------------------------
xr::UdpSocket    g_sock;
std::atomic<uint32_t> g_sessionId{0};      // read by the network thread too
std::atomic<uint32_t> g_txFreqKhz{0};      // radio the PTT keys, for voice packets
bool             g_connected    = false;
float            g_elapsed      = 0.f;
float            g_lastLogin    = -99.f;
float            g_lastRxTime   = -99.f;
bool             g_pttDown      = false;
std::string      g_status       = "not connected";
std::vector<std::string> g_chatLog;   // newest last, capped

XPLMWindowID     g_window       = nullptr;
XPLMFlightLoopID g_loop         = nullptr;
XPLMCommandRef   g_cmdPtt       = nullptr;
XPLMMenuID       g_menu         = nullptr;

void addChat(const std::string& s) {
    g_chatLog.push_back(s);
    if (g_chatLog.size() > 12) g_chatLog.erase(g_chatLog.begin());
}

// ---------------------------------------------------------------------------
// networking
// ---------------------------------------------------------------------------
int writeHeader(uint8_t* buf, uint8_t type, uint16_t payloadLen) {
    xr::Header h{};
    h.magic      = xr::kMagic;
    h.type       = type;
    h.version    = (uint8_t)xr::kProtoVersion;
    h.payloadLen = payloadLen;
    h.sessionId  = g_sessionId.load();
    memcpy(buf, &h, sizeof(h));
    return (int)sizeof(h);
}

void sendLogin() {
    uint8_t buf[sizeof(xr::Header) + sizeof(xr::LoginPayload)];
    int off = writeHeader(buf, xr::PT_LOGIN, sizeof(xr::LoginPayload));
    xr::LoginPayload p{};
    strncpy(p.callsign, g_cfg.callsign.c_str(), sizeof(p.callsign) - 1);
    strncpy(p.acIcao,   g_cfg.acIcao.c_str(),   sizeof(p.acIcao)   - 1);
    p.protoVer = xr::kProtoVersion;
    memcpy(buf + off, &p, sizeof(p));
    g_sock.send(buf, off + (int)sizeof(p));
    g_lastLogin = g_elapsed;
    g_status = "logging in to " + g_sock.endpoint() + "...";
}

void sendPosition() {
    xr::PositionPayload p{};
    p.lat         = dd(g_ref.lat);
    p.lon         = dd(g_ref.lon);
    p.altMslM     = (float)dd(g_ref.elev);
    p.headingTrue = fd(g_ref.psi);
    p.pitch       = fd(g_ref.theta);
    p.roll        = fd(g_ref.phi);
    p.gsMs        = fd(g_ref.gs);
    p.gearRatio   = firstOfArray(g_ref.gear);
    p.flapRatio   = fd(g_ref.flap);
    p.com1Khz     = (uint32_t)id(g_ref.com1);
    p.com2Khz     = (uint32_t)id(g_ref.com2);
    p.onGround    = id(g_ref.onGround) ? 1 : 0;

    uint8_t lights = 0;
    if (id(g_ref.ltNav))     lights |= xr::LT_NAV;
    if (id(g_ref.ltBeacon))  lights |= xr::LT_BEACON;
    if (id(g_ref.ltStrobe))  lights |= xr::LT_STROBE;
    if (id(g_ref.ltLanding)) lights |= xr::LT_LANDING;
    if (id(g_ref.ltTaxi))    lights |= xr::LT_TAXI;
    p.lights = lights;

    // audio_com_selection: 6 == COM1, 7 == COM2
    const int sel = id(g_ref.audioComSel);
    uint8_t tx = xr::TX_NONE;
    if (g_pttDown) tx = (sel == 7) ? xr::TX_COM2 : xr::TX_COM1;
    p.txRadio = tx;

    uint8_t rx = 0;
    if (id(g_ref.rxCom1)) rx |= xr::RX_COM1;
    if (id(g_ref.rxCom2)) rx |= xr::RX_COM2;
    if (rx == 0) rx = xr::RX_COM1;        // some aircraft do not wire the audio panel
    p.rxMask = rx;

    uint8_t buf[sizeof(xr::Header) + sizeof(p)];
    int off = writeHeader(buf, xr::PT_POSITION, sizeof(p));
    memcpy(buf + off, &p, sizeof(p));
    g_sock.send(buf, off + (int)sizeof(p));
}

void sendText(const std::string& text) {
    if (!g_connected || text.empty()) return;
    uint8_t buf[xr::kMaxPacket];
    uint16_t len = (uint16_t)std::min<size_t>(text.size(), 200);
    int off = writeHeader(buf, xr::PT_TEXT, (uint16_t)(sizeof(xr::TextHeader) + len));

    // Text does not use the PTT, so name the radio explicitly: whichever COM
    // the audio panel has selected for transmit. The server checks we really
    // are tuned there before relaying.
    xr::TextHeader th{};
    th.freqKhz     = (uint32_t)id(id(g_ref.audioComSel) == 7 ? g_ref.com2 : g_ref.com1);
    th.fromSession = g_sessionId;
    strncpy(th.from, g_cfg.callsign.c_str(), sizeof(th.from) - 1);
    th.textLen     = len;
    memcpy(buf + off, &th, sizeof(th));
    memcpy(buf + off + sizeof(th), text.data(), len);
    g_sock.send(buf, off + (int)sizeof(th) + len);
}

// Everything below arrives over UDP from a server we do not control, so it is
// checked before it reaches the renderer. Feeding NaN or a wild coordinate to
// XPMP2's SetLocation is not something the sim recovers from gracefully.
bool sane(const xr::TrafficEntry& e) {
    if (!std::isfinite(e.lat) || e.lat < -90.0  || e.lat > 90.0)  return false;
    if (!std::isfinite(e.lon) || e.lon < -180.0 || e.lon > 180.0) return false;
    if (!std::isfinite(e.altMslM) || e.altMslM < -1000.f || e.altMslM > 40000.f) return false;
    if (!std::isfinite(e.headingTrue) || !std::isfinite(e.pitch) ||
        !std::isfinite(e.roll))                                   return false;
    if (!std::isfinite(e.gsMs) || e.gsMs < 0.f || e.gsMs > 1500.f) return false;
    if (!std::isfinite(e.gearRatio) || !std::isfinite(e.flapRatio)) return false;
    return true;
}

float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Fixed-width C strings off the wire may be unterminated and may hold anything
// at all, and they end up drawn in the window and on aircraft labels.
std::string sanitizeText(const char* raw, size_t maxLen) {
    std::string out;
    for (size_t i = 0; i < maxLen && raw[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)raw[i];
        out += (c >= 32 && c < 127) ? (char)c : '?';
    }
    return out;
}

void handleTraffic(const uint8_t* payload, int len) {
    if (len < (int)sizeof(xr::TrafficHeader)) return;
    xr::TrafficHeader th{};
    memcpy(&th, payload, sizeof(th));

    int off = (int)sizeof(th);
    for (int i = 0; i < th.count; ++i) {
        if (off + (int)sizeof(xr::TrafficEntry) > len) break;
        xr::TrafficEntry e{};
        memcpy(&e, payload + off, sizeof(e));
        off += (int)sizeof(e);

        if (!sane(e)) {
            ++g_rejected;
            continue;               // drop this aircraft, keep the rest
        }

        const std::string cs = sanitizeText(e.callsign, sizeof(e.callsign));
        const std::string ic = sanitizeText(e.acIcao, sizeof(e.acIcao));

        Remote& r   = g_remote[e.sessionId];
        r.sid       = e.sessionId;
        r.callsign  = cs.empty() ? std::string("?") : cs;
        r.acIcao    = ic;
        r.lat       = e.lat;
        r.lon       = e.lon;
        r.altMslM   = e.altMslM;
        r.heading   = e.headingTrue;
        r.pitch     = e.pitch;
        r.roll      = e.roll;
        r.gsMs      = e.gsMs;
        r.gear      = clamp01(e.gearRatio);
        r.flap      = clamp01(e.flapRatio);
        r.lights    = e.lights;
        r.onGround  = e.onGround;
        r.txActive  = e.txActive;
        r.lastSeen  = g_elapsed;

        xr::RemoteState st;
        st.sid      = r.sid;
        st.callsign = r.callsign;
        st.acIcao   = r.acIcao;
        st.lat      = r.lat;
        st.lon      = r.lon;
        st.altFt    = r.altMslM * 3.28084f;
        st.heading  = r.heading;
        st.pitch    = r.pitch;
        st.roll     = r.roll;
        st.gsKt     = r.gsMs * 1.94384f;
        st.gear     = r.gear;
        st.flap     = r.flap;
        st.lights   = r.lights;
        st.onGround = r.onGround != 0;
        st.txActive = r.txActive != 0;
        xr::csl::upsert(st);
    }

    // forget aircraft the server stopped telling us about
    for (auto it = g_remote.begin(); it != g_remote.end();) {
        if (g_elapsed - it->second.lastSeen > 5.f) {
            xr::csl::remove(it->first);
            it = g_remote.erase(it);
        } else {
            ++it;
        }
    }
}

void handleText(const uint8_t* payload, int len) {
    if (len < (int)sizeof(xr::TextHeader)) return;
    xr::TextHeader th{};
    memcpy(&th, payload, sizeof(th));
    int textLen = std::min<int>(th.textLen, len - (int)sizeof(th));
    if (textLen < 0) return;

    // A server is not obliged to respect the same limits we send with, so the
    // receive side does its own clamping rather than trusting the header.
    const int kMaxShown = 200;
    if (textLen > kMaxShown) textLen = kMaxShown;

    const std::string from = sanitizeText(th.from, sizeof(th.from));
    const std::string msg  = sanitizeText(
        reinterpret_cast<const char*>(payload + sizeof(th)), (size_t)textLen);

    char line[320];
    snprintf(line, sizeof(line), "[%.3f] %s: %s",
             (double)(th.freqKhz % 1000000u) / 1000.0, from.c_str(), msg.c_str());
    addChat(line);
}

// ---------------------------------------------------------------------------
// network thread
// ---------------------------------------------------------------------------
// Voice cannot wait for the 5 Hz flight loop, and must not depend on the frame
// rate at all, so a dedicated thread owns the receive side of the socket.
// Voice frames are handed to the mixer immediately; everything else is queued
// for the main thread, which is the only place the sim may be touched.
std::thread                       g_netThread;
std::atomic<bool>                 g_netRun{false};
std::mutex                        g_inboxMx;
std::deque<std::vector<uint8_t>>  g_inbox;
const size_t                      kInboxCap = 256;

void handleVoicePacket(const uint8_t* payload, int len) {
    if (len < (int)sizeof(xr::VoiceHeader)) return;
    xr::VoiceHeader vh{};
    memcpy(&vh, payload, sizeof(vh));
    const int avail = len - (int)sizeof(vh);
    const int n = std::min<int>(vh.opusLen, avail);
    if (n <= 0) return;
    xr::voice::onIncomingFrame(vh.fromSession, vh.seq, payload + sizeof(vh), n);
}

void sendVoiceFrames() {
    static std::vector<xr::voice::OutFrame> frames;
    xr::voice::pollOutgoing(frames);
    if (frames.empty()) return;
    const uint32_t freq = g_txFreqKhz.load();
    if (freq == 0) return;                       // no radio selected: nothing to key
    uint8_t buf[xr::kMaxPacket];
    for (const auto& f : frames) {
        if (f.opus.empty() || f.opus.size() > 1000) continue;
        xr::VoiceHeader vh{};
        vh.freqKhz     = freq;
        vh.fromSession = g_sessionId.load();
        vh.seq         = f.seq;
        vh.opusLen     = (uint16_t)f.opus.size();
        const uint16_t payloadLen = (uint16_t)(sizeof(vh) + f.opus.size());
        int off = writeHeader(buf, xr::PT_VOICE, payloadLen);
        memcpy(buf + off, &vh, sizeof(vh));
        memcpy(buf + off + sizeof(vh), f.opus.data(), f.opus.size());
        g_sock.send(buf, off + (int)payloadLen);
    }
}

void netLoop() {
    uint8_t buf[xr::kMaxPacket];
    while (g_netRun.load()) {
        sendVoiceFrames();

        const int n = g_sock.recvWait(buf, sizeof(buf), 20);
        if (n < (int)sizeof(xr::Header)) continue;   // timeout, error, or runt

        xr::Header h{};
        memcpy(&h, buf, sizeof(h));
        if (h.magic != xr::kMagic || h.version != xr::kProtoVersion) continue;
        if ((int)(sizeof(h) + h.payloadLen) > n) continue;

        if (h.type == xr::PT_VOICE) {
            handleVoicePacket(buf + sizeof(h), h.payloadLen);
            continue;
        }
        std::lock_guard<std::mutex> lk(g_inboxMx);
        if (g_inbox.size() >= kInboxCap) g_inbox.pop_front();   // keep the newest
        g_inbox.emplace_back(buf, buf + sizeof(h) + h.payloadLen);
    }
}

void netStart() {
    if (g_netRun.load()) return;
    g_netRun.store(true);
    g_netThread = std::thread(netLoop);
}

void netStop() {
    if (!g_netRun.load()) return;
    g_netRun.store(false);
    if (g_netThread.joinable()) g_netThread.join();   // recvWait returns within 20 ms
}

// Main thread: dispatch what the network thread queued.
void pumpNetwork() {
    std::deque<std::vector<uint8_t>> batch;
    {
        std::lock_guard<std::mutex> lk(g_inboxMx);
        batch.swap(g_inbox);
    }
    for (const auto& pkt : batch) {
        xr::Header h{};
        memcpy(&h, pkt.data(), sizeof(h));
        const uint8_t* payload = pkt.data() + sizeof(h);
        g_lastRxTime = g_elapsed;

        switch (h.type) {
            case xr::PT_LOGIN_ACK: {
                if (h.payloadLen < sizeof(xr::LoginAckPayload)) break;
                xr::LoginAckPayload a{};
                memcpy(&a, payload, sizeof(a));
                g_sessionId.store(a.sessionId);
                g_connected = true;
                g_status    = "connected to " + g_sock.endpoint();
                logMsg("connected, session %u", (unsigned)g_sessionId.load());
                break;
            }
            case xr::PT_TRAFFIC: handleTraffic(payload, h.payloadLen); break;
            case xr::PT_TEXT:    handleText(payload, h.payloadLen);    break;
            case xr::PT_PONG:    break;
            default:             break;
        }
    }
}

// ---------------------------------------------------------------------------
// flight loop -- runs on the main thread, 5 Hz
// ---------------------------------------------------------------------------
float flightLoop(float elapsedSinceLast, float, int, void*) {
    g_elapsed += elapsedSinceLast;

    if (!g_sock.isOpen()) return 1.0f;

    pumpNetwork();
    xr::voice::tick();

    // Which COM the PTT keys, published for the network thread's voice packets.
    // audio_com_selection: 6 == COM1, 7 == COM2.
    g_txFreqKhz.store((uint32_t)id(id(g_ref.audioComSel) == 7 ? g_ref.com2 : g_ref.com1));

    if (!g_connected) {
        if (g_elapsed - g_lastLogin > 2.0f) sendLogin();   // retry until acked
        return 0.2f;
    }

    // the server drops us after 15 s of silence, so position doubles as keepalive
    sendPosition();

    if (g_elapsed - g_lastRxTime > 12.0f) {
        g_connected = false;
        g_sessionId.store(0);
        g_remote.clear();
        xr::csl::removeAll();
        g_status = "lost server, retrying...";
        logMsg("server went quiet, re-logging in");
    }

    return 0.2f;
}

// ---------------------------------------------------------------------------
// PTT command
// ---------------------------------------------------------------------------
int pttHandler(XPLMCommandRef, XPLMCommandPhase phase, void*) {
    bool changed = false;
    if (phase == xplm_CommandBegin)      { g_pttDown = true;  changed = true; }
    else if (phase == xplm_CommandEnd)   { g_pttDown = false; changed = true; }
    if (changed) {
        xr::voice::setTransmitting(g_pttDown);
        // Tell the server straight away rather than at the next 5 Hz tick, so
        // the [TX] label on our aircraft follows the key without a lag.
        if (g_connected) sendPosition();
    }
    return 0;   // let other plugins see it too
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
double distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 3440.065;
    double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * R * asin(std::min(1.0, sqrt(a)));
}

void drawWindow(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);

    float white[] = {1.f, 1.f, 1.f};
    float green[] = {0.4f, 1.f, 0.4f};
    float amber[] = {1.f, 0.8f, 0.3f};

    int y = t - 20;
    const int x = l + 10;

    XPLMDrawString(g_connected ? green : amber, x, y, (char*)g_status.c_str(),
                   nullptr, xplmFont_Proportional);
    y -= 16;
    char who[160];
    snprintf(who, sizeof(who), "%s as %s (%s)   Plugins > XRadio > Settings to change",
             g_sock.endpoint().empty() ? "no server" : g_sock.endpoint().c_str(),
             g_cfg.callsign.c_str(), g_cfg.acIcao.c_str());
    XPLMDrawString(white, x, y, who, nullptr, xplmFont_Basic);
    y -= 18;

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "COM1 %.3f   COM2 %.3f   %s",
             id(g_ref.com1) / 1000.0, id(g_ref.com2) / 1000.0,
             g_pttDown ? "** TX **" : "");
    XPLMDrawString(g_pttDown ? green : white, x, y, hdr, nullptr, xplmFont_Proportional);
    y -= 16;

    // Voice: device status, a mic meter while keyed, and who we are hearing.
    {
        std::string line = "Voice: " + xr::voice::status();
        if (g_pttDown) {
            const int bars = (int)(xr::voice::micLevel() * 10.f + 0.5f);
            line += "   MIC [";
            for (int i = 0; i < 10; ++i) line += (i < bars) ? '#' : '.';
            line += "]";
        }
        const auto rx = xr::voice::activeSpeakers();
        if (!rx.empty()) {
            line += "   RX:";
            for (uint32_t sid : rx) {
                auto it = g_remote.find(sid);
                line += " " + (it != g_remote.end() ? it->second.callsign : std::to_string(sid));
            }
        }
        const bool ok = xr::voice::available() && xr::voice::haveMicrophone();
        XPLMDrawString(!rx.empty() ? green : (ok ? white : amber), x, y,
                       (char*)line.c_str(), nullptr, xplmFont_Basic);
    }
    y -= 22;

    char title[96];
    if (xr::csl::available()) {
        snprintf(title, sizeof(title), "Traffic (%d)  ·  %d CSL models%s",
                 (int)g_remote.size(), xr::csl::cslModelCount(),
                 g_rejected ? "  · bad data rejected" : "");
    } else {
        snprintf(title, sizeof(title), "Traffic (%d)  ·  no 3D models%s",
                 (int)g_remote.size(), g_rejected ? "  · bad data rejected" : "");
    }
    XPLMDrawString(white, x, y, title, nullptr, xplmFont_Proportional);
    y -= 16;

    const double myLat = dd(g_ref.lat), myLon = dd(g_ref.lon);
    for (auto& kv : g_remote) {
        if (y < b + 100) break;
        const Remote& rm = kv.second;
        char line[160];
        snprintf(line, sizeof(line), "%-8s %-5s %5.0f ft  %5.1f nm  %3.0f kt %s",
                 rm.callsign.c_str(), rm.acIcao.c_str(), rm.altMslM * 3.28084f,
                 distanceNm(myLat, myLon, rm.lat, rm.lon), rm.gsMs * 1.94384f,
                 rm.txActive ? "<<TX" : "");
        XPLMDrawString(rm.txActive ? green : white, x, y, line, nullptr, xplmFont_Basic);
        y -= 14;
    }

    y -= 8;
    XPLMDrawString(white, x, y, (char*)"Radio", nullptr, xplmFont_Proportional);
    y -= 16;
    for (auto& msg : g_chatLog) {
        if (y < b + 10) break;
        XPLMDrawString(white, x, y, (char*)msg.c_str(), nullptr, xplmFont_Basic);
        y -= 14;
    }
}

// ---------------------------------------------------------------------------
// window placement
// ---------------------------------------------------------------------------
struct MonitorPick {
    bool found = false;
    int  l = 0, t = 0, r = 0, b = 0;
};

void monitorCb(int, int l, int t, int r, int b, void* refcon) {
    MonitorPick* m = (MonitorPick*)refcon;
    if (!m->found) { m->found = true; m->l = l; m->t = t; m->r = r; m->b = b; }
}

// X-Plane's "global desktop" spans every monitor, and its origin is the main
// monitor's lower-left -- so on a multi-monitor Linux setup the global bounds
// can start at a negative x, and a window placed at a fixed offset from them
// lands off-screen. Ask which monitors X-Plane is actually using, fall back to
// the global bounds, then clamp the result so the window is always reachable.
void safeWindowRect(int wantW, int wantH, int offsetX,
                    int* outL, int* outT, int* outR, int* outB) {
    MonitorPick m;
    XPLMGetAllMonitorBoundsGlobal(monitorCb, &m);   // full-screen monitors only

    int l, t, r, b;
    if (m.found) {
        l = m.l; t = m.t; r = m.r; b = m.b;         // windowed mode reports none
    } else {
        XPLMGetScreenBoundsGlobal(&l, &t, &r, &b);
    }

    // A nonsensical or degenerate report is worse than no report at all.
    if (r - l < 320 || t - b < 240) { l = 0; b = 0; r = 1280; t = 800; }

    const int maxW = r - l - 40;
    const int maxH = t - b - 80;
    int w = wantW < maxW ? wantW : maxW;
    int h = wantH < maxH ? wantH : maxH;
    if (w < 300) w = 300;
    if (h < 200) h = 200;

    int left = l + 50 + offsetX;
    int top  = t - 50;
    if (left + w > r - 10) left = r - 10 - w;       // keep the right edge on screen
    if (left < l + 10)     left = l + 10;           // ...and the left edge
    if (top - h < b + 10)  top  = b + 10 + h;       // keep the bottom on screen
    if (top > t - 10)      top  = t - 10;           // ...and the title bar

    *outL = left; *outT = top; *outR = left + w; *outB = top - h;
}

void createWindow() {
    int wl, wt, wr, wb;
    safeWindowRect(460, 400, 0, &wl, &wt, &wr, &wb);

    XPLMCreateWindow_t p{};
    p.structSize            = sizeof(p);
    p.left                  = wl;
    p.top                   = wt;
    p.right                 = wr;
    p.bottom                = wb;
    p.visible               = 1;
    p.drawWindowFunc        = drawWindow;
    p.handleMouseClickFunc  = [](XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; };
    p.handleRightClickFunc  = [](XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; };
    p.handleMouseWheelFunc  = [](XPLMWindowID, int, int, int, int, void*) { return 1; };
    p.handleKeyFunc         = [](XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {};
    p.handleCursorFunc      = [](XPLMWindowID, int, int, void*) -> XPLMCursorStatus {
        return xplm_CursorDefault;
    };
    p.layer                 = xplm_WindowLayerFloatingWindows;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;

    g_window = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(g_window, "XRadio");
    XPLMSetWindowResizingLimits(g_window, 320, 200, 900, 900);
}

// Drop the current session and log in again with whatever g_cfg says now.
void reconnect() {
    netStop();
    g_connected = false;
    g_sessionId.store(0);
    g_remote.clear();
    xr::csl::removeAll();
    {
        std::lock_guard<std::mutex> lk(g_inboxMx);
        g_inbox.clear();
    }
    std::string err;
    g_sock.close();
    if (!g_sock.open(g_cfg.host, (uint16_t)g_cfg.port, &err)) {
        g_status = "socket error: " + err;
        logMsg("%s", g_status.c_str());
    } else {
        sendLogin();
        netStart();
    }
}

// ---------------------------------------------------------------------------
// settings window -- edit host / port / callsign / type inside the sim
// ---------------------------------------------------------------------------
XPLMWindowID g_settingsWin = nullptr;
Config       g_edit;               // working copy while the window is open
int          g_focusField = -1;    // which field has keyboard focus, -1 none
std::string  g_settingsNote;       // one-line feedback under the buttons

struct Field {
    const char*  label;
    std::string* value;
    size_t       maxLen;
    bool         digitsOnly;
};

// The port field is stored as text while editing so the user can clear it.
std::string g_editPort;

Field fields[] = {
    {"Server host",   &g_edit.host,     63, false},
    {"Port",          &g_editPort,       5, true },
    {"Callsign",      &g_edit.callsign, 15, false},
    {"Aircraft type", &g_edit.acIcao,    7, false},
};
const int kNumFields = (int)(sizeof(fields) / sizeof(fields[0]));

// Geometry shared by draw and click handling.
const int kRowH      = 26;
const int kFirstRowY = 52;    // below the window top
const int kValueX    = 130;   // where the editable text starts

int rowY(int top, int i) { return top - kFirstRowY - i * kRowH; }
int buttonRowY(int top)  { return rowY(top, kNumFields) - 8; }

std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

void openSettings() {
    g_edit     = g_cfg;
    g_editPort = std::to_string(g_cfg.port);
    g_focusField = 0;
    g_settingsNote.clear();
    int l, t, r, b;
    safeWindowRect(360, 220, 520, &l, &t, &r, &b);
    XPLMSetWindowGeometry(g_settingsWin, l, t, r, b);
    XPLMSetWindowIsVisible(g_settingsWin, 1);
    XPLMBringWindowToFront(g_settingsWin);
    XPLMTakeKeyboardFocus(g_settingsWin);
}

void closeSettings() {
    g_focusField = -1;
    if (XPLMHasKeyboardFocus(g_settingsWin)) XPLMTakeKeyboardFocus(nullptr);
    XPLMSetWindowIsVisible(g_settingsWin, 0);
}

void applySettings() {
    const std::string host = trim(g_edit.host);
    const int port = atoi(g_editPort.c_str());
    if (host.empty())               { g_settingsNote = "Host cannot be empty";   return; }
    if (port < 1 || port > 65535)   { g_settingsNote = "Port must be 1-65535";  return; }
    if (trim(g_edit.callsign).empty()) { g_settingsNote = "Callsign cannot be empty"; return; }

    g_cfg.host     = host;
    g_cfg.port     = port;
    g_cfg.callsign = upper(trim(g_edit.callsign));
    g_cfg.acIcao   = upper(trim(g_edit.acIcao));
    if (g_cfg.acIcao.empty()) g_cfg.acIcao = "C172";

    saveConfig();
    closeSettings();
    reconnect();
}

void drawSettings(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);
    float white[] = {1.f, 1.f, 1.f};
    float grey[]  = {0.7f, 0.7f, 0.7f};
    float green[] = {0.4f, 1.f, 0.4f};
    float amber[] = {1.f, 0.8f, 0.3f};

    XPLMDrawString(white, l + 10, t - 24, (char*)"Click a field, type, Enter to save. Tab moves on.",
                   nullptr, xplmFont_Proportional);

    for (int i = 0; i < kNumFields; ++i) {
        const int y = rowY(t, i);
        const bool focused = (i == g_focusField);
        XPLMDrawString(focused ? white : grey, l + 10, y, (char*)fields[i].label,
                       nullptr, xplmFont_Proportional);
        std::string v = *fields[i].value;
        // a blinking cursor on the field being edited
        if (focused && ((int)(g_elapsed * 2.f) % 2 == 0)) v += "_";
        char line[96];
        snprintf(line, sizeof(line), "%s%s", focused ? "> " : "  ", v.c_str());
        XPLMDrawString(focused ? green : white, l + kValueX, y, line,
                       nullptr, xplmFont_Proportional);
    }

    const int by = buttonRowY(t);
    XPLMDrawString(green, l + 10,  by, (char*)"[ Save & reconnect ]", nullptr, xplmFont_Proportional);
    XPLMDrawString(grey,  l + 200, by, (char*)"[ Cancel ]",           nullptr, xplmFont_Proportional);

    if (!g_settingsNote.empty()) {
        XPLMDrawString(amber, l + 10, by - 22, (char*)g_settingsNote.c_str(),
                       nullptr, xplmFont_Proportional);
    }
}

int settingsClick(XPLMWindowID win, int x, int y, XPLMMouseStatus status, void*) {
    if (status != xplm_MouseDown) return 1;
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);

    for (int i = 0; i < kNumFields; ++i) {
        const int ry = rowY(t, i);
        if (y >= ry - 6 && y <= ry + 16) {
            g_focusField = i;
            XPLMTakeKeyboardFocus(win);
            return 1;
        }
    }
    const int by = buttonRowY(t);
    if (y >= by - 6 && y <= by + 16) {
        if (x >= l + 10 && x < l + 190)       applySettings();
        else if (x >= l + 200 && x < l + 290) closeSettings();
    }
    return 1;
}

void settingsKey(XPLMWindowID, char key, XPLMKeyFlags flags, char vk, void*, int losingFocus) {
    if (losingFocus) { g_focusField = -1; return; }
    if (!(flags & xplm_DownFlag)) return;
    if (g_focusField < 0 || g_focusField >= kNumFields) return;

    Field& f = fields[g_focusField];
    std::string& v = *f.value;
    const unsigned char uvk = (unsigned char)vk;
    const unsigned char c   = (unsigned char)key;

    if (uvk == XPLM_VK_BACK || c == 8) {
        if (!v.empty()) v.pop_back();
    } else if (uvk == XPLM_VK_RETURN || uvk == XPLM_VK_ENTER || c == '\r' || c == '\n') {
        applySettings();
    } else if (uvk == XPLM_VK_ESCAPE || c == 27) {
        closeSettings();
    } else if (uvk == XPLM_VK_TAB || c == '\t') {
        g_focusField = (g_focusField + 1) % kNumFields;
    } else if (c >= 32 && c < 127 && v.size() < f.maxLen) {
        if (f.digitsOnly && !isdigit(c)) return;
        if (c == ' ' && g_focusField != 0) return;   // no spaces in callsign / type
        v += (char)c;
    }
}

void createSettingsWindow() {
    int wl, wt, wr, wb;
    safeWindowRect(360, 220, 520, &wl, &wt, &wr, &wb);   // offset clear of the main window

    XPLMCreateWindow_t p{};
    p.structSize            = sizeof(p);
    p.left                  = wl;
    p.top                   = wt;
    p.right                 = wr;
    p.bottom                = wb;
    p.visible               = 0;
    p.drawWindowFunc        = drawSettings;
    p.handleMouseClickFunc  = settingsClick;
    p.handleRightClickFunc  = [](XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; };
    p.handleMouseWheelFunc  = [](XPLMWindowID, int, int, int, int, void*) { return 1; };
    p.handleKeyFunc         = settingsKey;
    p.handleCursorFunc      = [](XPLMWindowID, int, int, void*) -> XPLMCursorStatus {
        return xplm_CursorDefault;
    };
    p.layer                 = xplm_WindowLayerFloatingWindows;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;

    g_settingsWin = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(g_settingsWin, "XRadio Settings");
    XPLMSetWindowResizingLimits(g_settingsWin, 360, 220, 600, 400);
}

// Last resort if a window is dragged off-screen, or the monitor layout
// changed while X-Plane was running.
void resetWindowPositions() {
    int l, t, r, b;
    if (g_window) {
        safeWindowRect(460, 400, 0, &l, &t, &r, &b);
        XPLMSetWindowGeometry(g_window, l, t, r, b);
        XPLMSetWindowIsVisible(g_window, 1);
        XPLMBringWindowToFront(g_window);
    }
    if (g_settingsWin && XPLMGetWindowIsVisible(g_settingsWin)) {
        safeWindowRect(360, 220, 520, &l, &t, &r, &b);
        XPLMSetWindowGeometry(g_settingsWin, l, t, r, b);
        XPLMBringWindowToFront(g_settingsWin);
    }
    logMsg("window positions reset");
}

void menuHandler(void*, void* item) {
    intptr_t which = (intptr_t)item;
    if (which == 0 && g_window) {
        XPLMSetWindowIsVisible(g_window, !XPLMGetWindowIsVisible(g_window));
    } else if (which == 1) {
        openSettings();
    } else if (which == 2) {
        loadConfig();
        reconnect();
    } else if (which == 3) {
        resetWindowPositions();
    } else if (which == 4) {
        sendText("Hello from " + g_cfg.callsign);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// X-Plane plugin entry points
// ---------------------------------------------------------------------------
PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    strcpy(outName, "XRadio");
    strcpy(outSig,  "ee.doesvic.xradio");
    strcpy(outDesc, "Shared traffic and radio communication between X-Plane 12 pilots.");

    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    XPLMEnableFeature("XPLM_USE_NATIVE_WIDGET_WINDOWS", 1);

    loadConfig();
    findRefs();
    createWindow();
    createSettingsWindow();

    std::string cslErr;
    if (!xr::csl::init(pluginRootDir(), g_cfg.acIcao, &cslErr)) {
        logMsg("3D traffic unavailable: %s", cslErr.c_str());
    }

    std::string voiceErr;
#ifdef XRADIO_NULL_AUDIO
    const xr::voice::Mode voiceMode = xr::voice::Mode::Null;   // test builds: no hardware
#else
    const xr::voice::Mode voiceMode = xr::voice::Mode::Real;
#endif
    if (!xr::voice::init(voiceMode, &voiceErr)) {
        logMsg("voice unavailable: %s", voiceErr.c_str());
    } else {
        logMsg("voice: %s", xr::voice::status().c_str());
    }

    g_cmdPtt = XPLMCreateCommand("xradio/ptt", "XRadio: push to talk");
    XPLMRegisterCommandHandler(g_cmdPtt, pttHandler, 1, nullptr);

    int idx = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "XRadio", nullptr, 0);
    g_menu = XPLMCreateMenu("XRadio", XPLMFindPluginsMenu(), idx, menuHandler, nullptr);
    XPLMAppendMenuItem(g_menu, "Show / hide window",   (void*)0, 0);
    XPLMAppendMenuItem(g_menu, "Settings...",          (void*)1, 0);
    XPLMAppendMenuItem(g_menu, "Reconnect",            (void*)2, 0);
    XPLMAppendMenuItem(g_menu, "Reset window position", (void*)3, 0);
    XPLMAppendMenuItem(g_menu, "Send test message",    (void*)4, 0);

    XPLMCreateFlightLoop_t fl{};
    fl.structSize   = sizeof(fl);
    fl.phase        = xplm_FlightLoop_Phase_AfterFlightModel;
    fl.callbackFunc = flightLoop;
    g_loop = XPLMCreateFlightLoop(&fl);

    logMsg("started");
    return 1;
}

PLUGIN_API int XPluginEnable(void) {
    xr::csl::enable();

    std::string err;
    if (!g_sock.open(g_cfg.host, (uint16_t)g_cfg.port, &err)) {
        g_status = "socket error: " + err;
        logMsg("%s", g_status.c_str());
    } else {
        sendLogin();
        netStart();
    }
    XPLMScheduleFlightLoop(g_loop, 0.2f, 1);
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    xr::voice::setTransmitting(false);
    netStop();
    if (g_sock.isOpen() && g_connected) {
        uint8_t buf[sizeof(xr::Header)];
        writeHeader(buf, xr::PT_LOGOUT, 0);
        g_sock.send(buf, (int)sizeof(buf));
    }
    g_sock.close();
    g_connected = false;
    g_sessionId.store(0);
    g_remote.clear();
    xr::csl::disable();
    XPLMScheduleFlightLoop(g_loop, 0, 1);
}

PLUGIN_API void XPluginStop(void) {
    netStop();
    xr::voice::shutdown();
    xr::csl::shutdown();
    if (g_loop)   { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    if (g_window)      { XPLMDestroyWindow(g_window);      g_window = nullptr; }
    if (g_settingsWin) { XPLMDestroyWindow(g_settingsWin); g_settingsWin = nullptr; }
    if (g_cmdPtt) { XPLMUnregisterCommandHandler(g_cmdPtt, pttHandler, 1, nullptr); }
    if (g_menu)   { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
