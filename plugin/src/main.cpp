// XRadio -- X-Plane 12 multiplayer + radio plugin.
//
// Main thread (flight loop, 5 Hz): sends our position, drains the inbox of
// non-voice packets, drives XPMP2 and the windows.
// Network thread: receives everything; voice frames go straight to the mixer
// (voice.cpp), everything else is queued for the main thread. Also sends the
// frames the microphone produces.

#include "net.h"
#include "protocol.h"
#include "mathconst.h"
#include "server.h"
#include "upnp.h"
#include "brand.h"
#include "settings.h"
#include "joincode.h"
#include "smoothing.h"
#include "ui.h"
#include "clipboard.h"
#include "update.h"
#include "voice.h"
#include "weather.h"
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
#include <chrono>
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
using xr::Settings;

Settings    g_cfg;
std::string g_cfgPath;
int         g_focusCount = 0;      // focusable widgets the settings window drew
float       g_sendInterval = 0.2f; // seconds between position reports

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



void loadConfig() {
    resolveConfigPath();
    if (!xr::loadSettings(g_cfg, g_cfgPath)) {
        xr::saveSettings(g_cfg, g_cfgPath);     // first run: write the defaults
        logMsg("wrote default config to %s", g_cfgPath.c_str());
    }
    logMsg("config: %s:%d as %s (%s)", g_cfg.host.c_str(), g_cfg.port_i(),
           g_cfg.callsign.c_str(), g_cfg.acIcao.empty() ? "type from the sim" : g_cfg.acIcao.c_str());
}

// ---------------------------------------------------------------------------
// datarefs
// ---------------------------------------------------------------------------
struct Refs {
    XPLMDataRef lat, lon, elev, psi, theta, phi, gs, hpath, vh, yAgl;
    XPLMDataRef gear, flap, onGround;
    XPLMDataRef com1, com2, audioComSel, rxCom1, rxCom2, volCom1, volCom2;
    XPLMDataRef ltNav, ltBeacon, ltStrobe, ltLanding, ltTaxi;
    XPLMDataRef avionicsOn, com1Power, com2Power, busVolts;
    XPLMDataRef xpdrMode, xpdrCode, xpdrIdent;
    XPLMDataRef acfIcao, acfLivery;
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

// The first of these names this aircraft actually has, without complaining
// about the ones it does not: some things moved between the cockpit and
// cockpit2 namespaces and an aircraft is entitled to have only one.
XPLMDataRef firstRefOf(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (XPLMDataRef r = XPLMFindDataRef(n)) return r;
    }
    return nullptr;
}

void findRefs() {
    g_ref.lat      = findRef("sim/flightmodel/position/latitude");
    g_ref.lon      = findRef("sim/flightmodel/position/longitude");
    g_ref.elev     = findRef("sim/flightmodel/position/elevation");
    g_ref.psi      = findRef("sim/flightmodel/position/psi");
    g_ref.theta    = findRef("sim/flightmodel/position/theta");
    g_ref.phi      = findRef("sim/flightmodel/position/phi");
    g_ref.gs       = findRef("sim/flightmodel/position/groundspeed");
    g_ref.hpath    = findRef("sim/flightmodel/position/hpath");    // ground track, deg true
    g_ref.vh       = findRef("sim/flightmodel/position/vh_ind");   // vertical speed, m/s
    // How far our datum point sits above the terrain. On the ground that is
    // the height of the gear, which is exactly what we must not send -- see
    // sendPosition().
    g_ref.yAgl     = findRef("sim/flightmodel/position/y_agl");
    g_ref.gear     = findRef("sim/flightmodel2/gear/deploy_ratio");
    g_ref.flap     = findRef("sim/cockpit2/controls/flap_ratio");
    g_ref.onGround = findRef("sim/flightmodel/failures/onground_any");

    // 8.33 kHz variant reports the frequency in kHz, e.g. 118000 == 118.000 MHz
    g_ref.com1        = findRef("sim/cockpit2/radios/actuators/com1_frequency_hz_833");
    g_ref.com2        = findRef("sim/cockpit2/radios/actuators/com2_frequency_hz_833");
    g_ref.audioComSel = findRef("sim/cockpit2/radios/actuators/audio_com_selection");
    g_ref.rxCom1      = findRef("sim/cockpit2/radios/actuators/audio_selection_com1");
    g_ref.rxCom2      = findRef("sim/cockpit2/radios/actuators/audio_selection_com2");
    g_ref.volCom1     = findRef("sim/cockpit2/radios/actuators/audio_volume_com1");
    g_ref.volCom2     = findRef("sim/cockpit2/radios/actuators/audio_volume_com2");

    // A radio needs electricity. Without these an aircraft sitting cold and
    // dark would still be transmitting, which is the one thing every pilot
    // notices immediately.
    g_ref.avionicsOn = findRef("sim/cockpit2/switches/avionics_power_on");
    g_ref.com1Power  = findRef("sim/cockpit2/radios/actuators/com1_power");
    g_ref.com2Power  = findRef("sim/cockpit2/radios/actuators/com2_power");
    g_ref.busVolts   = findRef("sim/cockpit2/electrical/bus_volts");

    // The transponder. X-Plane 12 keeps it under cockpit2; older aircraft and
    // some add-ons only drive the original cockpit datarefs, and both use the
    // same numbering, so either will do. Tried quietly -- a missing name here
    // is expected, not a fault worth a warning in Log.txt.
    g_ref.xpdrMode  = firstRefOf({"sim/cockpit2/radios/actuators/transponder_mode",
                                  "sim/cockpit/radios/transponder_mode"});
    g_ref.xpdrCode  = firstRefOf({"sim/cockpit2/radios/actuators/transponder_code",
                                  "sim/cockpit/radios/transponder_code"});
    g_ref.xpdrIdent = firstRefOf({"sim/cockpit2/radios/actuators/transponder_id",
                                  "sim/cockpit/radios/transponder_id"});
    if (!g_ref.xpdrMode)
        logMsg("no transponder dataref in this aircraft -- treating it as mode C");

    g_ref.ltNav     = findRef("sim/cockpit2/switches/navigation_lights_on");
    g_ref.ltBeacon  = findRef("sim/cockpit2/switches/beacon_on");
    g_ref.ltStrobe  = findRef("sim/cockpit2/switches/strobe_lights_on");
    g_ref.ltLanding = findRef("sim/cockpit2/switches/landing_lights_on");
    g_ref.ltTaxi    = findRef("sim/cockpit2/switches/taxi_light_on");

    // What we are flying, so nobody has to type it: the ICAO type of the
    // loaded aircraft and the folder name of its livery.
    g_ref.acfIcao   = findRef("sim/aircraft/view/acf_ICAO");
    g_ref.acfLivery = findRef("sim/aircraft/view/acf_livery_path");

    if (g_missingRefs) {
        logMsg("%d dataref(s) missing -- those values will be sent as zero",
               g_missingRefs);
    }
}

float  fd(XPLMDataRef r) { return r ? XPLMGetDataf(r) : 0.f; }
double dd(XPLMDataRef r) { return r ? XPLMGetDatad(r) : 0.0; }
int    id(XPLMDataRef r) { return r ? XPLMGetDatai(r) : 0; }

// Highest voltage on any electrical bus. An aircraft sitting cold and dark
// reads zero everywhere; one with the master on reads its bus voltage.
float busVolts() {
    if (!g_ref.busVolts) return 0.f;
    float v[8] = {0.f};
    const int n = XPLMGetDatavf(g_ref.busVolts, v, 0, 8);
    float best = 0.f;
    for (int i = 0; i < n && i < 8; ++i) if (v[i] > best) best = v[i];
    return best;
}

// Can this COM actually transmit and receive? A radio needs the avionics
// bus, its own power switch, and volts behind them -- the same three things
// a pilot checks when the radio is dead. An aircraft that models none of
// this (no datarefs at all) is treated as powered, so an odd add-on does
// not go silent.
bool comPowered(int which) {
    if (!g_ref.avionicsOn && !g_ref.com1Power && !g_ref.busVolts) return true;
    if (g_ref.avionicsOn && !id(g_ref.avionicsOn)) return false;
    if (g_ref.busVolts && busVolts() < 8.f) return false;
    XPLMDataRef sw = (which == 2) ? g_ref.com2Power : g_ref.com1Power;
    if (sw && !id(sw)) return false;
    return true;
}

bool anyComPowered() { return comPowered(1) || comPowered(2); }

// Our own transponder, as it would answer an interrogation.
//
// It lives on the same avionics bus the radios do, so a cold and dark
// aircraft is not squawking -- which is the whole point of the thing being
// modelled rather than assumed. An aircraft with no transponder dataref at
// all is treated as squawking mode C, because the alternative is that every
// pilot in an add-on that does not model one vanishes from everybody's TCAS.
uint8_t ownXpdrMode() {
    if (!g_ref.xpdrMode) return comPowered(1) ? xr::XPDR_ALT : xr::XPDR_OFF;
    if (!comPowered(1) && !comPowered(2)) return xr::XPDR_OFF;
    const int m = id(g_ref.xpdrMode);
    if (m < 0 || m > xr::XPDR_TA_RA) return xr::XPDR_OFF;
    return (uint8_t)m;
}

uint16_t ownSquawk() {
    const int c = id(g_ref.xpdrCode);
    if (c < 0 || c > 7777) return 0;
    return xr::validSquawk((uint16_t)c) ? (uint16_t)c : 0;
}

// Human-readable, the way it is written on a panel and said on the radio.
const char* xpdrModeName(uint8_t mode) {
    switch (mode) {
        case xr::XPDR_OFF:     return "OFF";
        case xr::XPDR_STANDBY: return "STBY";
        case xr::XPDR_ON:      return "ON";
        case xr::XPDR_ALT:     return "ALT";
        case xr::XPDR_TEST:    return "TEST";
        case xr::XPDR_GROUND:  return "GND";
        case xr::XPDR_TA_ONLY: return "TA";
        default:               return "TA/RA";
    }
}

// A byte-array dataref as a trimmed string.
std::string sd(XPLMDataRef r, int maxLen) {
    if (!r) return "";
    char buf[1024] = {0};
    const int n = XPLMGetDatab(r, buf, 0, maxLen < (int)sizeof(buf) - 1 ? maxLen : (int)sizeof(buf) - 1);
    if (n <= 0) return "";
    std::string v(buf, (size_t)n);
    const size_t z = v.find('\0');
    if (z != std::string::npos) v.resize(z);
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

// The ICAO type to tell everyone: the settings override if set, else what
// the sim says about the loaded aircraft, else a default so the CSL matcher
// has something to work with.
std::string effectiveIcao() {
    if (!g_cfg.acIcao.empty()) return g_cfg.acIcao;
    std::string v = sd(g_ref.acfIcao, 40);
    std::string out;
    for (char c : v) {
        if (isalnum((unsigned char)c)) out += (char)toupper((unsigned char)c);
        if (out.size() >= 7) break;
    }
    return out.empty() ? std::string("C172") : out;
}

// The livery folder name: ".../liveries/Delta/" -> "Delta". Empty for the
// default paint. Printable ASCII only, it goes on the wire.
std::string effectiveLivery() {
    std::string path = sd(g_ref.acfLivery, 1023);
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string out;
    for (char c : name) {
        const unsigned char u = (unsigned char)c;
        if (u >= 32 && u < 127) out += c;
        if (out.size() >= 15) break;
    }
    return out;
}

float firstOfArray(XPLMDataRef r) {
    if (!r) return 0.f;
    float v = 0.f;
    if (XPLMGetDatavf(r, &v, 0, 1) < 1) return 0.f;
    return v;
}

// Sender-side timestamp for position reports. Only differences matter, so a
// steady clock since plugin start, wrapped into 32 bits, is all we need.
uint32_t nowMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// remote aircraft
// ---------------------------------------------------------------------------
struct Remote {
    uint32_t    sid = 0;
    std::string callsign, acIcao, livery;
    double      lat = 0, lon = 0;
    float       altMslM = 0, heading = 0, pitch = 0, roll = 0, gsMs = 0;
    float       gear = 0, flap = 0;
    uint8_t     lights = 0, onGround = 0, txActive = 0;
    uint16_t    squawk = 0;
    uint8_t     xpdrMode = 0;   // xr::XpdrMode, as their transponder answers
    uint8_t     xpdrIdent = 0;
    float       lastSeen = 0;   // seconds since plugin start
};

std::map<uint32_t, Remote> g_remote;
int g_rejected = 0;   // traffic entries dropped as implausible

// Defined with the hosting code further down; the main window needs it.
std::string shareAddress();
void reconnect();
void setPtt(bool down);
float g_lastWeatherSend = -1000.f;   // see sendWeather()
// When somebody else's sky last arrived. The server awards the weather claim
// to the first pilot who asks for it, and does not tell the losers -- but a
// loser can see it, because weather is relayed to everyone except whoever
// sent it. So: receiving means we are not the source, and should stop
// sending. It settles itself within one interval and costs no wire bytes.
float g_lastWeatherRx = -1000.f;
bool  someoneElseIsTheSky();
void handleWeather(const uint8_t* payload, int len);
double distanceNm(double lat1, double lon1, double lat2, double lon2);

// How well another pilot's radio reaches us, from distance against the VHF
// horizon the server uses (1.23 * (sqrt(h1) + sqrt(h2)) nm, 15 nm minimum).
// 1 up to about half the horizon, then falling to 0 at it: that is what the
// voice code turns into rising noise and the transmission breaking up.
float signalQuality(double distNm, float myAltFt, float theirAltFt) {
    const double h1 = myAltFt < 0.f ? 0.0 : (double)myAltFt;
    const double h2 = theirAltFt < 0.f ? 0.0 : (double)theirAltFt;
    double horizon = 1.23 * (sqrt(h1) + sqrt(h2));
    if (horizon < 15.0) horizon = 15.0;
    const double f = distNm / horizon;
    if (f <= 0.55) return 1.f;
    if (f >= 1.0)  return 0.f;
    const double t = (f - 0.55) / 0.45;
    return (float)(1.0 - t * t * (3.0 - 2.0 * t));   // smoothstep down
}

// ---------------------------------------------------------------------------
// plugin state
// ---------------------------------------------------------------------------
xr::UdpSocket    g_sock;
std::atomic<uint32_t> g_sessionId{0};      // read by the network thread too
std::atomic<uint32_t> g_txFreqKhz{0};      // radio the PTT keys, for voice packets
bool             g_connected    = false;
float            g_elapsed      = 0.f;
float            g_lastLogin    = -99.f;
float            g_firstLogin   = -1.f;     // when this connection attempt began
std::string      g_loginProblem;            // why the server will not have us
std::string      g_loginIcao, g_loginLivery;   // what the current session was logged in as
float            g_lastRxTime   = -99.f;
bool             g_pttDown      = false;
std::string      g_status       = "not connected";
std::vector<std::string> g_chatLog;   // newest last, capped

bool someoneElseIsTheSky() { return g_elapsed - g_lastWeatherRx < 30.f; }

// The one-line text input at the bottom of the main window. Only the main
// thread touches it, so keys are applied as they arrive.
std::string g_chatInput;
bool        g_chatFocus = false;
int         g_chatInputY = 0;          // where the draw put the row, for the click test

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
    g_loginIcao   = effectiveIcao();
    g_loginLivery = effectiveLivery();
    strncpy(p.callsign, g_cfg.callsign.c_str(), sizeof(p.callsign) - 1);
    strncpy(p.acIcao,   g_loginIcao.c_str(),    sizeof(p.acIcao)   - 1);
    strncpy(p.livery,   g_loginLivery.c_str(),  sizeof(p.livery)   - 1);
    strncpy(p.password, g_cfg.password.c_str(), sizeof(p.password) - 1);
    p.protoVer = xr::kProtoVersion;
    // Offer to be the authority on the sky. The server takes the first claim
    // and ignores the rest, so this is an offer, not an announcement -- which
    // is the only way it can work on a dedicated server, where there is no
    // host whose sim could be the obvious answer.
    p.flags = g_cfg.shareWeather ? xr::LF_WEATHER_SOURCE : 0;
    memcpy(buf + off, &p, sizeof(p));
    g_sock.send(buf, off + (int)sizeof(p));
    g_lastLogin = g_elapsed;
    if (g_firstLogin < 0.f) g_firstLogin = g_elapsed;
    if (g_loginProblem.empty()) g_status = "logging in to " + g_sock.endpoint() + "...";
}

void sendPosition() {
    xr::PositionPayload p{};
    p.lat         = dd(g_ref.lat);
    p.lon         = dd(g_ref.lon);
    // Altitude is the one field the receiver cannot simply draw as sent.
    // `elevation` is where our *datum point* is, which on the ground is a
    // metre or three up in the air, on top of the gear. The receiver then
    // hands it to XPMP2, which adds the CSL model's own VERT_OFFSET to stand
    // it on its wheels -- so the gear height gets counted twice and the model
    // hovers. What the receiver actually needs while we are on the ground is
    // the ground: send the terrain elevation and let each model's own offset
    // put its own wheels on it. In the air the datum is right and the
    // receiver drops the offset instead (see xpmp_bridge.cpp).
    // A missing y_agl reads 0, which leaves the old behaviour rather than a
    // new kind of wrong; the cap keeps a nonsense reading from burying the
    // model, which would look far worse than the hover it replaces.
    const bool  onGnd  = id(g_ref.onGround) != 0;
    float       gearUp = onGnd ? fd(g_ref.yAgl) : 0.f;
    if (!(gearUp > 0.f) || gearUp > 12.f) gearUp = 0.f;     // also catches NaN
    p.altMslM     = (float)(dd(g_ref.elev) - gearUp);
    p.headingTrue = fd(g_ref.psi);
    p.pitch       = fd(g_ref.theta);
    p.roll        = fd(g_ref.phi);
    p.gsMs        = fd(g_ref.gs);
    p.gearRatio   = firstOfArray(g_ref.gear);
    p.flapRatio   = fd(g_ref.flap);
    // A dead radio is on no frequency at all, so the server does not route
    // anyone's voice to us and other pilots do not see us listening.
    p.com1Khz     = comPowered(1) ? (uint32_t)id(g_ref.com1) : 0u;
    p.com2Khz     = comPowered(2) ? (uint32_t)id(g_ref.com2) : 0u;
    p.onGround    = onGnd ? 1 : 0;
    p.xpdrMode    = ownXpdrMode();
    p.squawk      = ownSquawk();
    p.xpdrIdent   = (xr::xpdrTransmitting(p.xpdrMode) && id(g_ref.xpdrIdent)) ? 1 : 0;

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
    if (tx == xr::TX_COM1 && !comPowered(1)) tx = xr::TX_NONE;
    if (tx == xr::TX_COM2 && !comPowered(2)) tx = xr::TX_NONE;
    p.txRadio = tx;

    uint8_t rx = 0;
    if (id(g_ref.rxCom1)) rx |= xr::RX_COM1;
    if (id(g_ref.rxCom2)) rx |= xr::RX_COM2;
    if (rx == 0) rx = xr::RX_COM1;        // some aircraft do not wire the audio panel
    if (!comPowered(1)) rx &= (uint8_t)~xr::RX_COM1;
    if (!comPowered(2)) rx &= (uint8_t)~xr::RX_COM2;
    p.rxMask = rx;

    p.timeMs    = nowMs();
    // Track is where we are going; heading is where the nose points. They
    // differ in a crosswind, and receivers extrapolate along track. Fall
    // back to heading if the sim does not expose the path.
    p.trackTrue = g_ref.hpath ? fd(g_ref.hpath) : p.headingTrue;
    p.vsMs      = fd(g_ref.vh);

    uint8_t buf[sizeof(xr::Header) + sizeof(p)];
    int off = writeHeader(buf, xr::PT_POSITION, sizeof(p));
    memcpy(buf + off, &p, sizeof(p));
    g_sock.send(buf, off + (int)sizeof(p));
}

// A frequency as it reads on the panel, except for guard -- which is the one
// frequency worth naming, because everybody hears it whatever they are tuned
// to and a pilot should know that before they say something on it.
std::string freqLabel(uint32_t khz) {
    if (xr::isGuard(khz)) return "GUARD 121.500";
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f", (double)(khz % 1000000u) / 1000.0);
    return buf;
}

// "@ESNA12 come to 118.1" -- a message for one pilot rather than a frequency.
// Split off the callsign and hand back the rest; an empty callsign means this
// is an ordinary radio call. Deliberately only at the very start of the line,
// so an "@" anywhere in a sentence is just an "@".
std::string splitDirect(const std::string& in, std::string* body) {
    *body = in;
    if (in.size() < 2 || in[0] != '@') return "";
    const size_t sp = in.find(' ');
    if (sp == std::string::npos || sp == 1) return "";
    std::string to = in.substr(1, sp - 1);
    for (char& c : to) c = (char)toupper((unsigned char)c);
    if (to.size() > 15) return "";
    for (char c : to) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') return "";
    }
    size_t at = in.find_first_not_of(' ', sp);
    if (at == std::string::npos) return "";      // a callsign and nothing to say
    *body = in.substr(at);
    return to;
}

void sendText(const std::string& text) {
    if (!g_connected || text.empty()) return;

    std::string body;
    const std::string to = splitDirect(text, &body);
    if (body.empty()) return;

    uint8_t buf[xr::kMaxPacket];
    uint16_t len = (uint16_t)std::min<size_t>(body.size(), 200);
    int off = writeHeader(buf, xr::PT_TEXT, (uint16_t)(sizeof(xr::TextHeader) + len));

    // Text does not use the PTT, so name the radio explicitly: whichever COM
    // the audio panel has selected for transmit. The server checks we really
    // are tuned there before relaying. A message addressed to a callsign
    // names no radio at all -- it is not a transmission.
    xr::TextHeader th{};
    th.freqKhz     = to.empty()
                     ? (uint32_t)id(id(g_ref.audioComSel) == 7 ? g_ref.com2 : g_ref.com1)
                     : 0u;
    th.fromSession = g_sessionId;
    strncpy(th.from, g_cfg.callsign.c_str(), sizeof(th.from) - 1);
    th.textLen     = len;
    strncpy(th.to, to.c_str(), sizeof(th.to) - 1);
    memcpy(buf + off, &th, sizeof(th));
    memcpy(buf + off + sizeof(th), body.data(), len);
    g_sock.send(buf, off + (int)sizeof(th) + len);

    // The server does not echo to the sender, so show it ourselves.
    char line[360];
    if (to.empty()) {
        snprintf(line, sizeof(line), "[%s] %s: %.*s", freqLabel(th.freqKhz).c_str(),
                 g_cfg.callsign.c_str(), (int)len, body.data());
    } else {
        snprintf(line, sizeof(line), "[direct to %s] %s: %.*s", to.c_str(),
                 g_cfg.callsign.c_str(), (int)len, body.data());
    }
    addChat(line);
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
    if (!std::isfinite(e.trackTrue) || !std::isfinite(e.vsMs) ||
        e.vsMs < -200.f || e.vsMs > 200.f)                        return false;
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
        const std::string lv = sanitizeText(e.livery, sizeof(e.livery));

        // A pilot who just joined should not spend up to ten seconds in
        // their own weather before the host's arrives.
        if (g_remote.find(e.sessionId) == g_remote.end())
            g_lastWeatherSend = -1000.f;

        Remote& r   = g_remote[e.sessionId];
        r.sid       = e.sessionId;
        r.callsign  = cs.empty() ? std::string("?") : cs;
        r.acIcao    = ic;
        r.livery    = lv;
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
        r.squawk    = xr::validSquawk(e.squawk) ? e.squawk : 0;
        r.xpdrMode  = e.xpdrMode <= xr::XPDR_TA_RA ? e.xpdrMode : (uint8_t)xr::XPDR_OFF;
        r.xpdrIdent = e.xpdrIdent ? 1 : 0;
        r.lastSeen  = g_elapsed;

        xr::RemoteState st;
        st.sid      = r.sid;
        st.callsign = r.callsign;
        st.acIcao   = r.acIcao;
        st.livery   = r.livery;
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
        st.timeMs   = e.timeMs;
        st.track    = e.trackTrue;
        st.vsFps    = e.vsMs * 3.28084f;
        st.squawk   = r.squawk;
        // Our own transponder has to be working for our TCAS to see anyone:
        // the interrogation comes from us. With ours off or in standby the
        // aircraft are still out of the window, but the TCAS display is
        // blank -- so their mode is passed on as standby, which is exactly
        // what XPMP2 reads as "not a TCAS target".
        st.xpdrMode = xr::xpdrTransmitting(ownXpdrMode()) ? r.xpdrMode
                                                          : (uint8_t)xr::XPDR_STANDBY;
        st.xpdrIdent = r.xpdrIdent != 0;
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

    const std::string to = sanitizeText(th.to, sizeof(th.to));

    // Three different things end up in this log and a pilot has to be able to
    // tell them apart at a glance: an ordinary call on a frequency, a call on
    // guard that everybody heard, and a message meant only for them.
    char line[360];
    if (!to.empty()) {
        snprintf(line, sizeof(line), "[direct] %s: %s", from.c_str(), msg.c_str());
    } else {
        snprintf(line, sizeof(line), "[%s] %s: %s", freqLabel(th.freqKhz).c_str(),
                 from.c_str(), msg.c_str());
    }
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
// The socket is opened on the network thread, because opening it means
// resolving the host name and a name that does not resolve (offline, a VPN
// that ate DNS, a typo) blocks for several seconds -- which on the main
// thread freezes the whole sim. Until it is open, the main thread only
// reads these two.
std::atomic<bool>                 g_sockReady{false};
std::mutex                        g_netErrMx;
std::string                       g_netErr;         // why the socket could not be opened
std::string                       g_netHost;        // what the thread should connect to
uint16_t                          g_netPort = 0;
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
    xr::voice::onIncomingFrame(vh.fromSession, vh.seq, payload + sizeof(vh), n, vh.freqKhz);
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
    {
        std::string err;
        if (!g_sock.open(g_netHost, g_netPort, &err)) {
            std::lock_guard<std::mutex> lk(g_netErrMx);
            g_netErr = err;
            return;
        }
        g_sockReady.store(true);
    }
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

// Open the socket and start receiving, to whatever the settings say now.
// Returns at once; the flight loop logs in once the socket reports ready.
void netStart() {
    if (g_netRun.load()) return;
    g_sockReady.store(false);
    {
        std::lock_guard<std::mutex> lk(g_netErrMx);
        g_netErr.clear();
    }
    g_netHost = g_cfg.activeHost();
    g_netPort = (uint16_t)g_cfg.activePort();
    g_firstLogin = -1.f;
    g_loginProblem.clear();
    g_lastLogin = -99.f;
    g_status = "connecting to " + g_netHost + ":" + std::to_string(g_netPort) + "...";
    g_netRun.store(true);
    g_netThread = std::thread(netLoop);
}

void netStop() {
    g_netRun.store(false);
    if (g_netThread.joinable()) g_netThread.join();   // recvWait returns within 20 ms
    g_sockReady.store(false);
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
                g_loginProblem.clear();
                g_status    = "connected to " + g_sock.endpoint();
                logMsg("connected, session %u, as %s%s%s", (unsigned)g_sessionId.load(),
                       g_loginIcao.c_str(), g_loginLivery.empty() ? "" : " / ",
                       g_loginLivery.c_str());
                break;
            }
            case xr::PT_LOGIN_REJECT: {
                if (h.payloadLen < sizeof(xr::LoginRejectPayload)) break;
                xr::LoginRejectPayload rj{};
                memcpy(&rj, payload, sizeof(rj));
                if (rj.reason == xr::RJ_PASSWORD) {
                    g_loginProblem = g_cfg.password.empty()
                        ? "this flight needs a password (Settings > Connection)"
                        : "wrong flight password";
                } else if (rj.reason == xr::RJ_VERSION) {
                    g_loginProblem = "the server runs a different XRadio version";
                } else {
                    g_loginProblem = "login refused";
                }
                g_status = g_loginProblem;
                logMsg("login refused: %s", g_loginProblem.c_str());
                break;
            }
            case xr::PT_TRAFFIC: handleTraffic(payload, h.payloadLen); break;
            case xr::PT_TEXT:    handleText(payload, h.payloadLen);    break;
            case xr::PT_WEATHER: handleWeather(payload, h.payloadLen); break;
            case xr::PT_PONG:    break;
            default:             break;
        }
    }
}

// The sky we are hosting, for everyone who is following. Sent on a slow
// clock: weather changes over minutes, and the packet is 452 bytes -- but
// straight away when someone joins, see handleTraffic.
static const float kWeatherIntervalS = 10.f;

void sendWeather() {
    xr::WeatherPayload w{};
    if (!xr::weather::read(w)) return;
    w.timeMs = (uint32_t)(g_elapsed * 1000.f);
    uint8_t buf[sizeof(xr::Header) + sizeof(w)];
    int off = writeHeader(buf, xr::PT_WEATHER, (uint16_t)sizeof(w));
    memcpy(buf + off, &w, sizeof(w));
    g_sock.send(buf, off + (int)sizeof(w));
}

void handleWeather(const uint8_t* payload, int len) {
    if (len != (int)sizeof(xr::WeatherPayload)) return;
    // Noted before the opt-out below: a pilot who is not following still
    // needs to know somebody else holds the claim, so they stop offering.
    g_lastWeatherRx = g_elapsed;
    if (!g_cfg.followWeather) return;
    xr::WeatherPayload w{};
    memcpy(&w, payload, sizeof(w));
    xr::weather::apply(w);
}

// ---------------------------------------------------------------------------
// flight loop -- runs on the main thread, every frame
// ---------------------------------------------------------------------------
// Receiving happens every frame so a report reaches the renderer the moment
// it arrives; draining at 5 Hz would add up to 200 ms of latency on top of
// the network's, and a jittery 200 ms at that. Sending stays at 5 Hz.
float g_lastSend = -99.f;
float g_lastAircraftCheck = 0.f;

float flightLoop(float elapsedSinceLast, float, int, void*) {
    g_elapsed += elapsedSinceLast;

    if (!g_sockReady.load()) {
        // Not open yet: either still resolving, or it failed and the thread
        // has left the reason for us.
        std::lock_guard<std::mutex> lk(g_netErrMx);
        if (!g_netErr.empty() && g_status.compare(0, 12, "socket error") != 0) {
            g_status = "socket error: " + g_netErr;
            logMsg("%s", g_status.c_str());
        }
        return 0.5f;
    }

    pumpNetwork();
    xr::voice::tick();

    // Tell the voice mixer how far away everyone is, so they sound like it.
    {
        const double myLat = dd(g_ref.lat), myLon = dd(g_ref.lon);
        const float  myAltFt = (float)(dd(g_ref.elev) * 3.28084);
        for (const auto& kv : g_remote) {
            const Remote& rm = kv.second;
            const double d = distanceNm(myLat, myLon, rm.lat, rm.lon);
            xr::voice::setSignalQuality(rm.sid, signalQuality(d, myAltFt, rm.altMslM * 3.28084f));
        }
    }

    // Which COM the PTT keys, published for the network thread's voice packets.
    // audio_com_selection: 6 == COM1, 7 == COM2.
    g_txFreqKhz.store((uint32_t)id(id(g_ref.audioComSel) == 7 ? g_ref.com2 : g_ref.com1));

    // The audio panel's volume knobs, so turning a radio down in the cockpit
    // turns it down in the headset. A missing dataref reads 0, which would
    // mute everything, so an absent knob counts as fully up.
    // A radio with no power is silent in both directions: zero volume here
    // stops anything already in the mixer, and the position packet has
    // already told the server we are not listening.
    const float vol1 = comPowered(1) ? (g_ref.volCom1 ? fd(g_ref.volCom1) : 1.f) : 0.f;
    const float vol2 = comPowered(2) ? (g_ref.volCom2 ? fd(g_ref.volCom2) : 1.f) : 0.f;
    xr::voice::setRadioVolumes((uint32_t)id(g_ref.com1), vol1,
                               (uint32_t)id(g_ref.com2), vol2);
    // Guard reaches every aeroplane with a working radio, on whichever is
    // turned up louder. With the avionics off it reaches this one too, and is
    // heard exactly as much as anything else is: not at all.
    xr::voice::setGuardGain(anyComPowered() ? std::max(vol1, vol2) : 0.f);

    // Switching the avionics off mid-transmission has to unkey us.
    if (g_pttDown && !anyComPowered()) setPtt(false);

    if (!g_connected) {
        // Retry until acked -- slowly once the server has said no, since the
        // answer will not change until the settings do -- and after a while
        // with no answer at all, say what that usually means.
        const float every = g_loginProblem.empty() ? 2.0f : 10.0f;
        if (g_elapsed - g_lastLogin > every) sendLogin();
        if (g_loginProblem.empty() && g_firstLogin >= 0.f && g_elapsed - g_firstLogin > 8.0f) {
            g_status = "no answer from " + g_sock.endpoint() +
                       " (offline, wrong address, or a different XRadio version)";
        }
        return -1.0f;
    }

    // Changed aircraft mid-session? Log in again so everyone sees the new one.
    // Not at startup: the sim is busy loading scenery and the pilot is not
    // reading the window yet. A test points XRADIO_UPDATE_URL somewhere
    // local and does not want to wait out the delay.
    static const float kUpdateDelayS = getenv("XRADIO_UPDATE_URL") ? 0.5f : 20.f;
    if (g_cfg.checkUpdates && g_elapsed > kUpdateDelayS)
        xr::update::checkAsync(xr::brand::version());

    if (g_elapsed - g_lastAircraftCheck > 3.0f) {
        g_lastAircraftCheck = g_elapsed;
        if (xr::relay::running() && g_cfg.hostUpnp) xr::upnp::tick("XRadio");
        if (effectiveIcao() != g_loginIcao || effectiveLivery() != g_loginLivery) {
            logMsg("aircraft changed to %s, re-logging in", effectiveIcao().c_str());
            reconnect();
            return -1.0f;
        }
    }

    if (g_connected && g_cfg.shareWeather && !someoneElseIsTheSky() &&
        g_elapsed - g_lastWeatherSend >= kWeatherIntervalS) {
        g_lastWeatherSend = g_elapsed;
        sendWeather();
    }

    // the server drops us after 15 s of silence, so position doubles as keepalive
    if (g_elapsed - g_lastSend >= g_sendInterval) {
        sendPosition();
        g_lastSend = g_elapsed;
    }

    if (g_elapsed - g_lastRxTime > 12.0f) {
        g_connected = false;
        g_sessionId.store(0);
        g_remote.clear();
        xr::csl::removeAll();
        g_lastWeatherRx   = -1000.f;
        g_lastWeatherSend = -1000.f;
        g_status = "lost server, retrying...";
        logMsg("server went quiet, re-logging in");
    }

    return -1.0f;   // every frame
}

// ---------------------------------------------------------------------------
// PTT command
// ---------------------------------------------------------------------------
void setPtt(bool down) {
    // Keying a radio that has no power should do nothing at all -- no
    // sidetone, no carrier for anyone else, no [TX] on our aircraft.
    if (down && !anyComPowered()) return;
    if (down == g_pttDown) return;
    g_pttDown = down;
    xr::voice::setTransmitting(g_pttDown);
    // Tell the server straight away rather than at the next 5 Hz tick, so
    // the [TX] label on our aircraft follows the key without a lag.
    if (g_connected) sendPosition();
}

int pttHandler(XPLMCommandRef, XPLMCommandPhase phase, void*) {
    if (phase == xplm_CommandBegin)      setPtt(true);
    else if (phase == xplm_CommandEnd)   setPtt(false);
    return 0;   // let other plugins see it too
}

// The push-to-talk key from the settings window. A hot key would only tell
// us about the press, and a radio needs the release too, so the key is
// sniffed instead -- after the windows have had it, so typing that letter
// into the chat box or a settings field does not key the transmitter.
int g_pttKeyVk = 0;

int pttKeySniffer(char, XPLMKeyFlags flags, char vk, void*) {
    const int k = (unsigned char)vk;
    if (g_pttKeyVk == 0 || k != g_pttKeyVk) return 1;      // not ours, pass it on
    if (flags & xplm_DownFlag) setPtt(true);
    else if (flags & xplm_UpFlag) setPtt(false);
    return 0;                                              // eaten
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
double distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 3440.065;
    double p1 = lat1 * xr::kPi / 180.0, p2 = lat2 * xr::kPi / 180.0;
    double dp = p2 - p1, dl = (lon2 - lon1) * xr::kPi / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * R * asin(std::min(1.0, sqrt(a)));
}

// X-Plane lets the pilot drag this window down to 320 px wide. A line that
// does not fit has to be cut here: XPLMDrawString happily paints past the
// window's edge and over the cockpit, which looks like a broken plugin.
void drawFit(float* col, int x, int y, int right, const std::string& text,
             XPLMFontID font) {
    const int avail = right - x - 8;
    if (avail <= 0) return;
    if (XPLMMeasureString(font, text.c_str(), (int)text.size()) <= (float)avail) {
        XPLMDrawString(col, x, y, (char*)text.c_str(), nullptr, font);
        return;
    }
    // Trim from a proportional first guess rather than one character at a
    // time: a traffic list of twenty aircraft redraws every frame.
    std::string t = text;
    const float per = XPLMMeasureString(font, t.c_str(), (int)t.size()) / (float)t.size();
    size_t keep = (size_t)((float)avail / (per > 0.f ? per : 7.f));
    if (keep > t.size()) keep = t.size();
    t.resize(keep);
    while (!t.empty() &&
           XPLMMeasureString(font, (t + "..").c_str(), (int)t.size() + 2) > (float)avail) {
        t.pop_back();
    }
    t += "..";
    XPLMDrawString(col, x, y, (char*)t.c_str(), nullptr, font);
}

void drawWindow(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);

    float white[] = {1.f, 1.f, 1.f};
    float green[] = {0.4f, 1.f, 0.4f};
    float amber[] = {1.f, 0.8f, 0.3f};
    float grey[]  = {0.78f, 0.82f, 0.88f};

    int y = t - 20;
    const int x = l + 10;

    y -= xr::brand::draw(x, y, true);              // the mark is the first row

    drawFit(g_connected ? green : amber, x, y, r, g_status, xplmFont_Proportional);
    y -= 16;
    char who[200];
    snprintf(who, sizeof(who), "%s as %s (%s%s%s)  ·  Plugins > XRadio > Settings",
             g_sock.endpoint().empty() ? "no server" : g_sock.endpoint().c_str(),
             g_cfg.callsign.c_str(), effectiveIcao().c_str(),
             effectiveLivery().empty() ? "" : " ", effectiveLivery().c_str());
    drawFit(white, x, y, r, who, xplmFont_Basic);
    y -= 16;

    if (xr::relay::running()) {
        const xr::relay::Status st = xr::relay::status();
        char hostLine[200];
        snprintf(hostLine, sizeof(hostLine),
                 "Hosting  ·  %d connected  ·  friends type %s",
                 st.clients, shareAddress().c_str());
        drawFit(green, x, y, r, hostLine, xplmFont_Basic);
        y -= 16;
    }

    // Whose sky we are flying. Worth a row of its own: when it is not the one
    // the pilot expected, this line is the difference between "it is broken"
    // and "somebody else got there first".
    if (g_connected) {
        const char* sky = "my own";
        if (someoneElseIsTheSky())
            sky = g_cfg.followWeather ? "the flight's" : "the flight's (not following)";
        else if (g_cfg.shareWeather)
            sky = "mine, shared with the flight";
        char line[120];
        snprintf(line, sizeof(line), "Weather and time: %s", sky);
        drawFit(grey, x, y, r, line, xplmFont_Basic);
        y -= 16;
    }
    y -= 2;

    {
        // Two lines, because the address has to be readable enough to type:
        // nothing in an X-Plane window is clickable.
        const xr::update::Info up = xr::update::latest();
        if (up.newer) {
            char line[200];
            snprintf(line, sizeof(line), "XRadio v%s is out -- you are on v%s",
                     up.latest.c_str(), xr::brand::version());
            drawFit(amber, x, y, r, line, xplmFont_Basic);
            y -= 16;
            std::string where = up.url;
            const size_t scheme = where.find("://");
            if (scheme != std::string::npos) where = where.substr(scheme + 3);
            const size_t tag = where.find("/releases/");
            if (tag != std::string::npos) where = where.substr(0, tag) + "/releases";
            drawFit(amber, x + 14, y, r, where, xplmFont_Basic);
            y -= 18;
        }
    }

    char hdr[160];
    const bool p1 = comPowered(1), p2 = comPowered(2);
    snprintf(hdr, sizeof(hdr), "COM1 %.3f%s   COM2 %.3f%s   %s",
             id(g_ref.com1) / 1000.0, p1 ? "" : " (no power)",
             id(g_ref.com2) / 1000.0, p2 ? "" : " (no power)",
             g_pttDown ? "** TX **" : "");
    drawFit(g_pttDown ? green : white, x, y, r, hdr, xplmFont_Proportional);
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
                // signal bars, like a phone: five for next door, one at the horizon
                const int bars = 1 + (int)(xr::voice::signalQualityOf(sid) * 4.f + 0.5f);
                line += " [";
                for (int i = 0; i < 5; ++i) line += (i < bars) ? '|' : '.';
                line += "]";
            }
        }
        const bool ok = xr::voice::available() && xr::voice::haveMicrophone();
        drawFit(!rx.empty() ? green : (ok ? white : amber), x, y, r, line,
                xplmFont_Basic);
    }
    y -= 22;

    // What TCAS can actually see, which is not the same as who is out there.
    // An aircraft answers an interrogation only with its transponder above
    // standby, and the interrogation comes from us, so ours has to be up too.
    // Everyone else is still drawn out of the window and still on the radio:
    // neither needs a transponder, and that is the point of modelling it.
    const uint8_t myXpdr   = ownXpdrMode();
    const bool    myXpdrUp = xr::xpdrTransmitting(myXpdr);
    std::vector<const Remote*> contacts;
    if (myXpdrUp) {
        for (const auto& kv : g_remote)
            if (xr::xpdrTransmitting(kv.second.xpdrMode)) contacts.push_back(&kv.second);
    }

    char ownXpdrText[40];
    if (myXpdrUp) {
        snprintf(ownXpdrText, sizeof(ownXpdrText), "XPDR %04u %s%s",
                 (unsigned)ownSquawk(), xpdrModeName(myXpdr),
                 id(g_ref.xpdrIdent) ? " ID" : "");
    } else {
        snprintf(ownXpdrText, sizeof(ownXpdrText), "TCAS off -- XPDR %s",
                 xpdrModeName(myXpdr));
    }

    char title[160];
    if (xr::csl::available()) {
        const std::string src = xr::csl::cslModelSource();
        snprintf(title, sizeof(title), "Traffic (%d)  ·  %s  ·  %d CSL models%s%s%s",
                 (int)contacts.size(), ownXpdrText, xr::csl::cslModelCount(),
                 src.empty() ? "" : " from ", src.c_str(),
                 g_rejected ? "  · bad data rejected" : "");
    } else {
        snprintf(title, sizeof(title), "Traffic (%d)  ·  %s  ·  no 3D models%s",
                 (int)contacts.size(), ownXpdrText,
                 g_rejected ? "  · bad data rejected" : "");
    }
    drawFit(myXpdrUp ? white : amber, x, y, r, title, xplmFont_Proportional);
    y -= 16;

    const double myLat = dd(g_ref.lat), myLon = dd(g_ref.lon);
    for (const Remote* rp : contacts) {
        if (y < b + 100) break;
        const Remote& rm = *rp;
        // Mode A answers with an identity and nothing else. Your TCAS knows
        // it is there and roughly where, but not how high -- a bearing-only
        // target. Inventing an altitude for it would be a lie in the one
        // place a pilot is entitled to trust the display.
        char alt[16];
        if (xr::xpdrReportsAltitude(rm.xpdrMode))
            snprintf(alt, sizeof(alt), "%5.0f ft", rm.altMslM * 3.28084f);
        else
            snprintf(alt, sizeof(alt), "  no alt");
        char line[200];
        snprintf(line, sizeof(line), "%-8s %-5s %s  %5.1f nm  %3.0f kt  %04u%s%s",
                 rm.callsign.c_str(), rm.acIcao.c_str(), alt,
                 distanceNm(myLat, myLon, rm.lat, rm.lon), rm.gsMs * 1.94384f,
                 (unsigned)rm.squawk, rm.xpdrIdent ? " ID" : "",
                 rm.txActive ? "  <<TX" : "");
        const bool emergency = rm.squawk == 7500 || rm.squawk == 7600 ||
                               rm.squawk == 7700;
        drawFit(rm.txActive ? green : (emergency ? amber : white), x, y, r, line,
                xplmFont_Basic);
        y -= 14;
    }

    y -= 8;
    drawFit(white, x, y, r, "Radio", xplmFont_Proportional);
    y -= 16;
    // Newest messages win the space; the input row at the bottom is reserved.
    const int inputY = b + 12;
    for (auto& msg : g_chatLog) {
        if (y < inputY + 22) break;
        drawFit(white, x, y, r, msg, xplmFont_Basic);
        y -= 14;
    }

    // Type here, Enter sends on the radio the audio panel has selected.
    g_chatInputY = inputY;
    char prompt[260];
    const bool blink = ((int)(g_elapsed * 2.f) % 2) == 0;
    if (g_chatFocus) {
        snprintf(prompt, sizeof(prompt), "Say: > %s%s", g_chatInput.c_str(), blink ? "_" : "");
    } else if (!g_chatInput.empty()) {
        snprintf(prompt, sizeof(prompt), "Say:   %s", g_chatInput.c_str());
    } else {
        // The hint is the only place @CALLSIGN is discoverable, and guard is
        // worth naming next to it: between them they are the answer to "I do
        // not know what frequency anyone is on".
        snprintf(prompt, sizeof(prompt),
                 "Say:   (click to type, Enter to send  ·  @CALLSIGN for one pilot"
                 "  ·  121.500 is heard by all)");
    }
    drawFit(g_chatFocus ? green : (g_chatInput.empty() ? amber : white), x, inputY, r,
            prompt, xplmFont_Basic);
}

// Clicking the "Say:" row takes the keyboard; clicking anywhere else in the
// window gives it back, so the sim's own key bindings keep working.
int mainWindowClick(XPLMWindowID win, int, int y, XPLMMouseStatus status, void*) {
    if (status != xplm_MouseDown) return 1;
    if (y >= g_chatInputY - 6 && y <= g_chatInputY + 15) {
        g_chatFocus = true;
        XPLMTakeKeyboardFocus(win);
    } else if (g_chatFocus) {
        g_chatFocus = false;
        XPLMTakeKeyboardFocus(nullptr);
    }
    return 1;
}

void mainWindowKey(XPLMWindowID, char key, XPLMKeyFlags flags, char vk, void*, int losingFocus) {
    if (losingFocus) { g_chatFocus = false; return; }
    if (!(flags & xplm_DownFlag) || !g_chatFocus) return;
    const unsigned char uvk = (unsigned char)vk;
    const unsigned char ch  = (unsigned char)key;
    if (uvk == XPLM_VK_RETURN || uvk == XPLM_VK_ENTER || ch == '\r' || ch == '\n') {
        if (!g_chatInput.empty()) {
            if (g_connected) {
                sendText(g_chatInput);
            } else {
                addChat("(not connected -- nothing sent)");
            }
            g_chatInput.clear();
        }
        return;
    }
    if (uvk == XPLM_VK_ESCAPE || ch == 27) {
        g_chatInput.clear();
        g_chatFocus = false;
        XPLMTakeKeyboardFocus(nullptr);
        return;
    }
    if (uvk == XPLM_VK_BACK || ch == 8) {
        if (!g_chatInput.empty()) g_chatInput.pop_back();
        return;
    }
    if (ch >= 32 && ch < 127 && g_chatInput.size() < 200) g_chatInput += (char)ch;
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
    safeWindowRect(520, 400, 0, &wl, &wt, &wr, &wb);

    XPLMCreateWindow_t p{};
    p.structSize            = sizeof(p);
    p.left                  = wl;
    p.top                   = wt;
    p.right                 = wr;
    p.bottom                = wb;
    p.visible               = 1;
    p.drawWindowFunc        = drawWindow;
    p.handleMouseClickFunc  = mainWindowClick;
    p.handleRightClickFunc  = [](XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; };
    p.handleMouseWheelFunc  = [](XPLMWindowID, int, int, int, int, void*) { return 1; };
    p.handleKeyFunc         = mainWindowKey;
    p.handleCursorFunc      = [](XPLMWindowID, int, int, void*) -> XPLMCursorStatus {
        return xplm_CursorDefault;
    };
    p.layer                 = xplm_WindowLayerFloatingWindows;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;

    g_window = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(g_window, "XRadio");
    XPLMSetWindowResizingLimits(g_window, 320, 200, 900, 900);
}

// ---------------------------------------------------------------------------
// hosting
// ---------------------------------------------------------------------------
// The whole point: one pilot switches this on and the others type their
// address. No VPS, no Python, no terminal. The plugin runs the same relay
// server that server.py runs, on its own thread, and its own client connects
// to it over the loopback.
std::string g_hostNote;      // why hosting is not working, if it is not
// What the last click on a Copy button did, shown for a few seconds. A
// clipboard operation that silently did nothing is worse than no button.
std::string g_copyNote;
float       g_copyNoteAt = -100.f;
bool        g_copyOk = false;

void applyHosting() {
    const uint16_t port = (uint16_t)g_cfg.hostPort_i();

    if (!g_cfg.hostEnabled) {
        if (xr::relay::running()) {
            xr::relay::stop();
            xr::upnp::releaseAsync();
            logMsg("hosting stopped");
        }
        g_hostNote.clear();
        return;
    }

    xr::relay::stop();
    std::string err;
    if (!xr::relay::start(port, g_cfg.password, &err)) {
        g_hostNote = "cannot host: " + err;
        logMsg("%s", g_hostNote.c_str());
        return;
    }
    g_hostNote.clear();
    logMsg("hosting on port %u", (unsigned)port);

    xr::upnp::clear();
    xr::upnp::requestAsync(port, "XRadio", g_cfg.hostUpnp);
}

// The address on this network, for friends on the same LAN.
std::string lanAddress() {
    const std::string lan = xr::localAddress();
    const std::string port = std::to_string(g_cfg.hostPort_i());
    return (lan.empty() ? std::string("<this computer>") : lan) + ":" + port;
}

// What the main window tells the hosting pilot to send their friends. Only
// an address the internet can reach counts; until the port is known to be
// open it is the LAN one, marked as such, so nobody passes on a 192.168
// address to a friend across town.
// The two things worth copying, plain: the address a friend types into their
// Connection tab, and the short code that stands in for it. shareAddress()
// above is the same information dressed for reading, which is not what you
// want on a clipboard.
std::string shareAddressPlain() {
    const xr::upnp::Result u = xr::upnp::latest();
    if (u.mapped && !u.externalIp.empty())
        return u.externalIp + ":" + std::to_string(g_cfg.hostPort_i());
    return lanAddress();
}

std::string shareJoinCode() {
    const xr::upnp::Result u = xr::upnp::latest();
    if (u.externalIp.empty()) return "";
    return xr::joincode::encode(u.externalIp, (uint16_t)g_cfg.hostPort_i());
}

std::string shareAddress() {
    const xr::upnp::Result u = xr::upnp::latest();
    if (u.mapped && !u.externalIp.empty()) {
        const std::string code = xr::joincode::encode(u.externalIp, (uint16_t)g_cfg.hostPort_i());
        return u.externalIp + ":" + std::to_string(g_cfg.hostPort_i()) +
               (code.empty() ? "" : "  (code " + code + ")");
    }
    return lanAddress() + " (your network only)";
}

// Drop the current session and log in again with whatever g_cfg says now.
// Tell the server we are going, so it frees the session now instead of
// waiting out its 15 s timeout. Without this, saving a settings change left
// the old session alive on the server and the pilot watched their previous
// callsign sitting in their own traffic list at 0.0 nm.
void sendLogout() {
    if (!g_sock.isOpen() || !g_connected) return;
    uint8_t buf[sizeof(xr::Header)];
    writeHeader(buf, xr::PT_LOGOUT, 0);
    g_sock.send(buf, (int)sizeof(buf));
}

void reconnect() {
    netStop();
    sendLogout();
    g_connected = false;
    g_sessionId.store(0);
    g_remote.clear();
    xr::csl::removeAll();
    // A new connection knows nothing about who holds the sky, so offer again
    // rather than staying quiet on the strength of the old flight's answer.
    g_lastWeatherRx  = -1000.f;
    g_lastWeatherSend = -1000.f;
    {
        std::lock_guard<std::mutex> lk(g_inboxMx);
        g_inbox.clear();
    }
    g_sock.close();
    netStart();
}

// ---------------------------------------------------------------------------
// settings window
// ---------------------------------------------------------------------------
// Tabbed, driven by the descriptor table in settings.h so a new setting shows
// up here automatically. Drawn with the text widgets in ui.h, which means the
// whole thing is a list of strings the test harness can read back.
XPLMWindowID g_settingsWin = nullptr;
Settings     g_edit;               // working copy while the window is open
int          g_tab = 0;
std::string  g_settingsNote;       // one-line feedback under the buttons
bool         g_captureKey = false; // the PTT-key row is waiting for a key press
xr::ui::Ctx  g_ui;
std::vector<std::string> g_micList, g_outList;

// Keystrokes arrive whenever X-Plane feels like it, several per frame under a
// low frame rate, but the widgets only exist during a draw. Queue them raw and
// let the draw apply them in order; see drainKeys() for why Enter and Tab are
// not acted on the moment they arrive.
struct PendingKey { char ch; unsigned char vk; };
std::vector<PendingKey> g_keyQueue;
enum { kSpecialNone = 0, kSpecialApply, kSpecialClose, kSpecialTab };
int g_specialKey = kSpecialNone;

std::string upper(std::string v) {
    for (auto& c : v) c = (char)toupper((unsigned char)c);
    return v;
}

void refreshDeviceLists() {
    g_micList.clear();
    g_outList.clear();
    for (const auto& d : xr::voice::listDevices(true))  g_micList.push_back(d.name);
    for (const auto& d : xr::voice::listDevices(false)) g_outList.push_back(d.name);
}

// Push the live settings into the subsystems that care about them. Called on
// save and at startup, so there is one path rather than two.
void applyLiveSettings() {
    xr::voice::setVolume(g_cfg.volume);
    xr::voice::setSidetone(g_cfg.sidetone);
    xr::voice::setHiss(g_cfg.hiss);
    xr::voice::setRadioFilter(g_cfg.radioFilter);
    xr::Smoother::setDefaultPlayout(g_cfg.smoothMs / 1000.0);
    g_pttKeyVk = g_cfg.pttKey;
    xr::csl::setTrafficVisible(g_cfg.showTraffic);
    xr::csl::setLabels(g_cfg.showLabels, g_cfg.labelDistNm);
    g_sendInterval = 1.0f / (g_cfg.reportHz < 1.f ? 1.f : g_cfg.reportHz);
}

void openSettings() {
    g_edit = g_cfg;
    g_settingsNote.clear();
    g_captureKey = false;
    g_ui.focus = -1;
    g_keyQueue.clear();
    g_specialKey = kSpecialNone;
    refreshDeviceLists();

    int l, t, r, b;
    safeWindowRect(560, 330, 520, &l, &t, &r, &b);
    XPLMSetWindowGeometry(g_settingsWin, l, t, r, b);
    XPLMSetWindowIsVisible(g_settingsWin, 1);
    XPLMBringWindowToFront(g_settingsWin);
    XPLMTakeKeyboardFocus(g_settingsWin);
}

void closeSettings() {
    g_ui.focus = -1;
    g_captureKey = false;
    g_keyQueue.clear();
    if (XPLMHasKeyboardFocus(g_settingsWin)) XPLMTakeKeyboardFocus(nullptr);
    XPLMSetWindowIsVisible(g_settingsWin, 0);
}

void applySettings() {
    g_edit.callsign = upper(trim(g_edit.callsign));
    g_edit.acIcao   = upper(trim(g_edit.acIcao));
    g_edit.host     = trim(g_edit.host);
    g_edit.password = trim(g_edit.password);

    const std::string problem = xr::validate(g_edit);
    if (!problem.empty()) {
        g_settingsNote = problem;
        return;
    }

    const bool netChanged = (g_edit.host != g_cfg.host) ||
                            (g_edit.port != g_cfg.port) ||
                            (g_edit.callsign != g_cfg.callsign) ||
                            (g_edit.acIcao != g_cfg.acIcao) ||
                            (g_edit.password != g_cfg.password) ||
                            (g_edit.hostEnabled != g_cfg.hostEnabled) ||
                            (g_edit.hostPort != g_cfg.hostPort) ||
                            // Who owns the sky is claimed at login, so
                            // changing our mind means logging in again --
                            // otherwise the setting appears to do nothing
                            // until the next flight.
                            (g_edit.shareWeather != g_cfg.shareWeather);
    const bool hostChanged = (g_edit.hostEnabled != g_cfg.hostEnabled) ||
                             (g_edit.hostPort != g_cfg.hostPort) ||
                             (g_edit.password != g_cfg.password) ||
                             (g_edit.hostUpnp != g_cfg.hostUpnp);
    const bool devChanged = (g_edit.micDevice != g_cfg.micDevice) ||
                            (g_edit.outDevice != g_cfg.outDevice);

    g_cfg = g_edit;
    xr::saveSettings(g_cfg, g_cfgPath);
    applyLiveSettings();
    // Start or stop the built-in server before reconnecting, so the client
    // has something to connect to by the time it tries.
    if (hostChanged) applyHosting();

    if (devChanged) {
        std::string err;
        if (!xr::voice::reopenDevices(g_cfg.micDevice, g_cfg.outDevice, &err)) {
            logMsg("could not switch audio device: %s", err.c_str());
        } else {
            logMsg("audio devices: mic '%s', out '%s'",
                   xr::voice::currentMic().c_str(), xr::voice::currentOutput().c_str());
        }
    }
    closeSettings();
    if (netChanged) reconnect();
}

// Hand the widgets every key up to the next Enter/Escape/Tab, and remember
// that one for the end of the frame. Acting on Enter the instant it arrives
// would save the field as it was *before* the characters typed just ahead of
// it in the same frame -- the last thing typed would silently not be saved.
void drainKeys() {
    g_specialKey = kSpecialNone;
    g_ui.keys.clear();

    size_t taken = 0;
    for (; taken < g_keyQueue.size(); ++taken) {
        const PendingKey& k = g_keyQueue[taken];
        int special = kSpecialNone;
        if (k.vk == XPLM_VK_RETURN || k.vk == XPLM_VK_ENTER ||
            k.ch == '\r' || k.ch == '\n') {
            special = kSpecialApply;
        } else if (k.vk == XPLM_VK_ESCAPE || k.ch == 27) {
            special = kSpecialClose;
        } else if (k.vk == XPLM_VK_TAB || k.ch == '\t') {
            special = kSpecialTab;
        }
        if (special != kSpecialNone) {
            g_specialKey = special;
            ++taken;                 // consumed; the rest waits for next frame
            break;
        }
        g_ui.pushKey(k.ch, k.vk);
    }
    g_keyQueue.erase(g_keyQueue.begin(),
                     g_keyQueue.begin() + (std::ptrdiff_t)taken);
}

void drawSettings(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);

    drainKeys();

    g_ui.blink = ((int)(g_elapsed * 2.f) % 2) == 0;
    g_ui.begin(l, t, r, b, 24);

    xr::brand::draw(l + 24, t - 24);
    g_ui.nextRow();

    const char* names[xr::kNumTabs];
    for (int i = 0; i < xr::kNumTabs; ++i) names[i] = xr::tabName(i);
    xr::ui::tabs(g_ui, names, xr::kNumTabs, g_tab);
    g_ui.nextRow();

    auto fields = xr::describe(g_edit);
    for (auto& f : fields) {
        if (f.tab != g_tab) continue;
        switch (f.kind) {
            case xr::Kind::Text:
                xr::ui::textField(g_ui, f.label, *(std::string*)f.ptr, f.maxLen, f.digitsOnly);
                break;
            case xr::Kind::Bool:
                xr::ui::toggle(g_ui, f.label, *(bool*)f.ptr);
                break;
            case xr::Kind::Slider:
                xr::ui::slider(g_ui, f.label, *(float*)f.ptr, f.lo, f.hi, f.unit,
                               std::string(f.unit) == "%");
                break;
            case xr::Kind::Choice: {
                const bool isMic = std::string(f.key) == "mic";
                xr::ui::choice(g_ui, f.label, *(std::string*)f.ptr,
                               isMic ? g_micList : g_outList, "system default");
                break;
            }
            case xr::Kind::KeyBind:
                xr::ui::keybind(g_ui, f.label, xr::keyName(*(int*)f.ptr), g_captureKey);
                break;
        }
    }

    // The hosting tab is mostly status: what to send your friends, and
    // whether the router co-operated. Guessing at this is the thing that
    // makes people give up, so it is all spelled out.
    if (g_tab == 3) {
        g_ui.nextRow();
        const xr::relay::Status st = xr::relay::status();
        if (!g_hostNote.empty()) {
            xr::ui::text(g_ui, g_hostNote.c_str(), 3);
        } else if (!st.running) {
            xr::ui::text(g_ui, "Not hosting. Switch it on and save; your friends", 1);
            xr::ui::text(g_ui, "then put your address in their Connection tab.", 1);
        } else {
            char line[192];
            snprintf(line, sizeof(line), "Running on port %u  ·  %d connected",
                     (unsigned)st.port, st.clients);
            xr::ui::text(g_ui, line, 2);

            const xr::upnp::Result u = xr::upnp::latest();
            const std::string port = std::to_string(st.port);
            if (u.mapped && !u.externalIp.empty()) {
                snprintf(line, sizeof(line), "Friends type:  %s:%s", u.externalIp.c_str(), port.c_str());
                xr::ui::text(g_ui, line, 0);
                const std::string code = xr::joincode::encode(u.externalIp, st.port);
                if (!code.empty()) {
                    snprintf(line, sizeof(line), "   or the join code:  %s   (goes in their Server host field)",
                             code.c_str());
                    xr::ui::text(g_ui, line, 2);
                }
                snprintf(line, sizeof(line), "Router opened the port (%s)%s",
                         u.router.empty() ? "UPnP" : u.router.c_str(),
                         u.leaseSeconds ? ", an hour at a time" : "");
                xr::ui::text(g_ui, line, 2);
            } else if (u.mapped) {
                snprintf(line, sizeof(line), "Router opened the port (%s) but did not say its address",
                         u.router.empty() ? "UPnP" : u.router.c_str());
                xr::ui::text(g_ui, line, 2);
                snprintf(line, sizeof(line), "Friends type:  <your public address>:%s", port.c_str());
                xr::ui::text(g_ui, line, 0);
            } else if (xr::upnp::busy()) {
                xr::ui::text(g_ui, g_cfg.hostUpnp ? "Asking the router to open the port..."
                                                  : "Looking up your public address...", 1);
                snprintf(line, sizeof(line), "Friends type:  %s   (on your network)", lanAddress().c_str());
                xr::ui::text(g_ui, line, 0);
            } else {
                // The port is not open. Say exactly what will and will not
                // work and what to do about it: a 192.168 address handed to
                // a friend across town is the classic dead end.
                if (!g_cfg.hostUpnp) {
                    xr::ui::text(g_ui, "Router not asked to open the port.", 1);
                } else if (u.doubleNat) {
                    xr::ui::text(g_ui, "Your router is itself behind another NAT (mobile or shared", 3);
                    xr::ui::text(g_ui, "internet): hosting over the internet cannot work from here.", 3);
                } else if (u.done) {
                    snprintf(line, sizeof(line), "Port not opened: %s", u.error.c_str());
                    xr::ui::text(g_ui, line, 3);
                }
                snprintf(line, sizeof(line), "Friends type:  %s   (on your network)", lanAddress().c_str());
                xr::ui::text(g_ui, line, 0);
                if (!u.doubleNat) {
                    snprintf(line, sizeof(line), "Over the internet: forward UDP %s on your router to %s,",
                             port.c_str(), xr::localAddress().c_str());
                    xr::ui::text(g_ui, line, 1);
                    snprintf(line, sizeof(line), "then friends type  %s:%s",
                             u.externalIp.empty() ? "<your public address>" : u.externalIp.c_str(),
                             port.c_str());
                    xr::ui::text(g_ui, line, 1);
                    const std::string code = u.externalIp.empty() ? "" : xr::joincode::encode(u.externalIp, st.port);
                    if (!code.empty()) {
                        snprintf(line, sizeof(line), "   or the join code:  %s", code.c_str());
                        xr::ui::text(g_ui, line, 1);
                    }
                    // Forwarding is not always possible: an ISP-managed box
                    // with no settings page, a carrier's NAT, a network
                    // somebody else runs. Saying only "forward the port"
                    // leaves those pilots stuck with no idea there is a way.
                    xr::ui::text(g_ui, "If you cannot change the router: let a friend host instead, or put", 1);
                    xr::ui::text(g_ui, "everyone on a VPN like Tailscale or ZeroTier and host on the address", 1);
                    xr::ui::text(g_ui, "it gives you -- no forwarding needed. server/ also runs on a VPS.", 1);
                    // Saying "use a VPN" and stopping there is how a pilot
                    // gives up. The guide is the rest of that sentence.
                    xr::ui::text(g_ui,
                        "Step by step:  github.com/Anviktor2411/xradio/blob/main/docs/vpn.md", 2);
                }
            }

            if (!st.callsigns.empty()) {
                std::string who = "Here now:";
                for (const auto& c : st.callsigns) who += " " + c;
                xr::ui::text(g_ui, who.c_str(), 0);
            }

            // Nothing drawn in an X-Plane window can be selected with the
            // mouse, so up to here a join code is something you read out
            // loud or photograph. These put it on the clipboard instead.
            const std::string addr = shareAddressPlain();
            const std::string code = shareJoinCode();
            std::vector<const char*>        labels;
            std::vector<const std::string*> values;
            if (!code.empty()) {
                labels.push_back("Copy join code");
                values.push_back(&code);
            }
            // lanAddress() says "<this computer>" when it cannot find one,
            // which is a sentence, not an address: nothing to paste.
            if (!addr.empty() && addr.find('<') == std::string::npos) {
                labels.push_back("Copy address");
                values.push_back(&addr);
            }
            if (!labels.empty()) {
                g_ui.nextRow();
                const int hit = xr::ui::buttons(g_ui, labels.data(), (int)labels.size());
                if (hit >= 0) {
                    std::string err;
                    g_copyOk = xr::clipboard::set(*values[(size_t)hit], &err);
                    g_copyNote = g_copyOk ? "Copied:  " + *values[(size_t)hit]
                                          : "Could not copy -- " + err;
                    g_copyNoteAt = g_elapsed;
                }
                if (!g_copyNote.empty() && g_elapsed - g_copyNoteAt < 8.f)
                    xr::ui::text(g_ui, g_copyNote.c_str(), g_copyOk ? 2 : 3);
            }
        }
    }

    // Live feedback on the audio tab: a meter beats guessing.
    if (g_tab == 1) {
        g_ui.nextRow();
        char meter[96];
        const int bars = (int)(xr::voice::micLevel() * 16.f + 0.5f);
        char bar[20];
        bar[0] = '[';
        for (int i = 0; i < 16; ++i) bar[i + 1] = i < bars ? '#' : '.';
        bar[17] = ']'; bar[18] = '\0';
        snprintf(meter, sizeof(meter), "Mic level %s  %s", bar,
                 g_pttDown ? "(keyed)" : "(hold PTT to test)");
        xr::ui::text(g_ui, meter, bars > 0 ? 2 : 1);
        xr::ui::text(g_ui, ("Voice: " + xr::voice::status()).c_str(), 1);
    }

    g_ui.nextRow();
    // A row is held back for the note that a refused save produces. It has
    // to be drawn above the buttons, but its text only exists once the Save
    // button has been handled -- hence reserving the row, then coming back
    // to it.
    const int noteRow = g_ui.y;
    g_ui.nextRow();

    // Measured here, before the clamp on the next line moves the row back up
    // on screen. Measuring after it would report little more than the height
    // the window already has, so a window a pilot had made short would claw
    // back a row per frame and stop as soon as the buttons fitted -- with the
    // content above them still cut off, and no way to get it back.
    const int wantHeight = g_ui.heightFor(g_ui.y);

    // Whatever happened above, the buttons have to be on screen and
    // clickable: a settings window you cannot save or close is a trap.
    if (g_ui.y < b + xr::ui::Ctx::kRowH) g_ui.y = b + 12;
    const int buttonRow = g_ui.y;
    static const char* btns[] = {"Save & apply", "Cancel"};
    const int hit = xr::ui::buttons(g_ui, btns, 2);
    if (hit == 0) applySettings();
    else if (hit == 1) closeSettings();

    if (!g_settingsNote.empty()) {
        g_ui.y = noteRow > buttonRow ? noteRow : buttonRow + xr::ui::Ctx::kRowH;
        xr::ui::text(g_ui, g_settingsNote.c_str(), 3);
    }

    // The Hosting tab says a great deal more than the others when a router
    // will not cooperate, so a window sized for Connection cuts it off.
    // Grow to fit what is actually being drawn -- never shrink, because the
    // pilot may have sized it deliberately.
    // Only when the content itself changed size, not on every frame it does
    // not fit: growing between a draw and the click that draw produced moves
    // the row out from under the pilot's cursor.
    // Remembered as a pair: what we asked for, and the height we asked it
    // from. Keyed on the wanted height alone, a pilot who drags the window
    // short while a tall tab is open never gets the content back -- the tab
    // has not changed size, so the same answer is refused for ever.
    static int g_grownTo = 0, g_grownFrom = 0;
    if (g_ui.clipped && (wantHeight != g_grownTo || (t - b) != g_grownFrom)) {
        const int want = wantHeight;
        g_grownTo = want;
        g_grownFrom = t - b;
        if (want > t - b) {
            int sl, st, sr, sb;
            XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
            int newBottom = t - want;
            if (newBottom < sb + 20) newBottom = sb + 20;
            if (newBottom < b) XPLMSetWindowGeometry(g_settingsWin, l, t, r, newBottom);
        }
    }

    g_focusCount = g_ui.index;
    g_ui.endInput();

    // Now that the fields hold what was typed, act on the key that ended the
    // burst. Tab needs g_focusCount, which only this pass knows.
    switch (g_specialKey) {
        case kSpecialApply: applySettings(); break;
        case kSpecialClose: closeSettings(); break;
        case kSpecialTab:
            if (g_focusCount > 0) g_ui.focus = (g_ui.focus + 1) % g_focusCount;
            break;
        default: break;
    }
    g_specialKey = kSpecialNone;
}

int settingsClick(XPLMWindowID win, int x, int y, XPLMMouseStatus status, void*) {
    if (status != xplm_MouseDown) return 1;
    g_ui.clicked = true;
    g_ui.clickX = x;
    g_ui.clickY = y;
    XPLMTakeKeyboardFocus(win);
    return 1;   // the next draw consumes it
}

void settingsKey(XPLMWindowID, char key, XPLMKeyFlags flags, char vk, void*, int losingFocus) {
    if (losingFocus) { g_ui.focus = -1; g_keyQueue.clear(); g_captureKey = false; return; }
    if (!(flags & xplm_DownFlag)) return;
    if (g_captureKey) {
        // Whatever was pressed becomes the PTT key; Escape means "none".
        const int k = (unsigned char)vk;
        g_edit.pttKey = (k == XPLM_VK_ESCAPE || k == 0) ? 0 : k;
        g_captureKey = false;
        return;
    }
    // Everything, Enter and Escape included, goes through the queue so the
    // draw applies it in the order it was typed. 256 is far more than a
    // frame's worth; the cap only matters if the window stops drawing.
    if (g_keyQueue.size() < 256) g_keyQueue.push_back({key, (unsigned char)vk});
}

void createSettingsWindow() {
    int wl, wt, wr, wb;
    safeWindowRect(560, 330, 520, &wl, &wt, &wr, &wb);

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
    XPLMSetWindowResizingLimits(g_settingsWin, 520, 280, 900, 700);
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
        safeWindowRect(560, 330, 520, &l, &t, &r, &b);
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
    xr::weather::init();
    createWindow();
    createSettingsWindow();

    std::string cslErr;
    if (!xr::csl::init(pluginRootDir(), effectiveIcao(), g_cfg.cslPath, &cslErr)) {
        logMsg("3D traffic unavailable: %s", cslErr.c_str());
    }

    std::string voiceErr;
#ifdef XRADIO_NULL_AUDIO
    const xr::voice::Mode voiceMode = xr::voice::Mode::Null;   // test builds: no hardware
#else
    const xr::voice::Mode voiceMode = xr::voice::Mode::Real;
#endif
    if (!xr::voice::init(voiceMode, g_cfg.micDevice, g_cfg.outDevice, &voiceErr)) {
        logMsg("voice unavailable: %s", voiceErr.c_str());
    } else {
        logMsg("voice: %s", xr::voice::status().c_str());
    }
    applyLiveSettings();
    applyHosting();

    g_cmdPtt = XPLMCreateCommand("xradio/ptt", "XRadio: push to talk");
    XPLMRegisterCommandHandler(g_cmdPtt, pttHandler, 1, nullptr);
    XPLMRegisterKeySniffer(pttKeySniffer, 0 /* after windows */, nullptr);

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
    xr::csl::setLabels(g_cfg.showLabels, g_cfg.labelDistNm);

    if (!g_cfg.autoConnect) {
        g_status = "not connected (autoconnect off)";
        XPLMScheduleFlightLoop(g_loop, -1.0f, 1);
        return 1;
    }

    netStart();
    XPLMScheduleFlightLoop(g_loop, -1.0f, 1);   // every frame
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    xr::voice::setTransmitting(false);
    netStop();
    sendLogout();
    g_sock.close();
    g_connected = false;
    g_sessionId.store(0);
    g_remote.clear();
    xr::csl::disable();
    XPLMScheduleFlightLoop(g_loop, 0, 1);
}

PLUGIN_API void XPluginStop(void) {
    netStop();
    xr::relay::stop();
    xr::upnp::releaseAsync();      // give the router's port back if we can
    xr::upnp::shutdown();
    xr::update::shutdown();
    xr::voice::shutdown();
    xr::csl::shutdown();
    if (g_loop)   { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    if (g_window)      { XPLMDestroyWindow(g_window);      g_window = nullptr; }
    if (g_settingsWin) { XPLMDestroyWindow(g_settingsWin); g_settingsWin = nullptr; }
    if (g_cmdPtt) { XPLMUnregisterCommandHandler(g_cmdPtt, pttHandler, 1, nullptr); }
    XPLMUnregisterKeySniffer(pttKeySniffer, 0, nullptr);
    if (g_menu)   { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
