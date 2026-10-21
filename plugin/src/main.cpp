// XRadio -- multiplayer + radio plugin for X-Plane 11.50 and later, 12 included.
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
#include "radio.h"
#include "shadow.h"
#include "terrain.h"
#include "ui.h"
#include "clipboard.h"
#include "update.h"
#include "voice.h"
#include "weather.h"
#include "xpmp_bridge.h"
#include "tower.h"
#include "simbrief.h"
#include "acftype.h"
#include "tcasread.h"
#ifdef XRADIO_UI_IMGUI
#  include "ui2/host.h"
#  include "ui2/imgui_assert.h"
#endif

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"
#include "XPLMPlanes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// X-Plane 11.50 is the floor: it is the first sim XPMP2 draws in, and every
// API used here exists in it. The X-Plane 12 APIs (XPLM400) are deliberately
// not enabled, so the plugin cannot grow a dependency on them by accident
// and stop loading in X-Plane 11. What 12 adds -- the region weather that
// sky sharing rides on -- is found by dataref at run time instead.
#if !defined(XPLM303)
#  error "Build with -DXPLM200 -DXPLM210 -DXPLM300 -DXPLM301 -DXPLM303 (X-Plane 11.50 or later)"
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
    XPLMDataRef com1Hz, com2Hz;         // the 25 kHz radios of a sim before 11.30
    XPLMDataRef ltNav, ltBeacon, ltStrobe, ltLanding, ltTaxi;
    XPLMDataRef avionicsOn, com1Power, com2Power, busVolts;
    XPLMDataRef xpdrMode, xpdrCode, xpdrIdent;
    XPLMDataRef acfIcao, acfLivery, acfTail, acfDescrip;
    // X-Plane 12.4+ TCAS II (null on older sims: no TCAS state is sent then)
    XPLMDataRef tcasAdvisory, tcasMessage, tcasBands, tcasThreat, tcasModeS;
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

    // The 8.33 kHz variant reports the frequency in kHz, e.g. 118000 ==
    // 118.000 MHz. It arrived in X-Plane 11.30; a sim older than that only
    // has the 25 kHz one, in units of 10 kHz (11880 == 118.800 MHz), which
    // comKhz() scales. Tried quietly: one or the other is always there.
    g_ref.com1        = firstRefOf({"sim/cockpit2/radios/actuators/com1_frequency_hz_833"});
    g_ref.com2        = firstRefOf({"sim/cockpit2/radios/actuators/com2_frequency_hz_833"});
    g_ref.com1Hz      = nullptr;
    g_ref.com2Hz      = nullptr;
    if (!g_ref.com1 || !g_ref.com2) {
        g_ref.com1Hz  = findRef("sim/cockpit2/radios/actuators/com1_frequency_hz");
        g_ref.com2Hz  = findRef("sim/cockpit2/radios/actuators/com2_frequency_hz");
        logMsg("no 8.33 kHz radio datarefs (X-Plane before 11.30) -- using the 25 kHz ones");
    }
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
    g_ref.acfTail   = findRef("sim/aircraft/view/acf_tailnum");
    g_ref.acfDescrip = findRef("sim/aircraft/view/acf_descrip");
    // the pilot's own TCAS (PART D): looked up quietly, X-Plane 11 and 12 before 12.4 do not have them
    g_ref.tcasAdvisory = XPLMFindDataRef("sim/cockpit2/tcas/indicators/tcas_active_advisory");
    g_ref.tcasMessage  = XPLMFindDataRef("sim/cockpit2/tcas/indicators/tcas_message");
    g_ref.tcasBands    = XPLMFindDataRef("sim/cockpit2/tcas/indicators/tcas_vs_bands");
    g_ref.tcasThreat   = XPLMFindDataRef("sim/cockpit2/tcas/targets/threat");
    g_ref.tcasModeS    = XPLMFindDataRef("sim/cockpit2/tcas/targets/modeS_id");

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

// What COM1 or COM2 is tuned to, in kHz, from whichever dataref this sim has.
uint32_t comKhz(int which) {
    XPLMDataRef fine   = (which == 2) ? g_ref.com2   : g_ref.com1;
    XPLMDataRef coarse = (which == 2) ? g_ref.com2Hz : g_ref.com1Hz;
    if (fine)   return (uint32_t)id(fine);
    if (coarse) return (uint32_t)id(coarse) * 10u;
    return 0;
}

// The radio the audio panel has selected for transmit: 6 is COM1, 7 is COM2.
int txCom() { return id(g_ref.audioComSel) == 7 ? 2 : 1; }

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

// ---------------------------------------------------------------------------
// the aircraft we fly: detected from the sim (acf_ICAO, the .acf file, its
// description), checked against ICAO Doc 8643, known add-ons mapped -- see
// acftype.h. Main thread only.
// ---------------------------------------------------------------------------
xr::acftype::Doc8643 g_doc8643;
xr::acftype::Info    g_acf;
std::string          g_acfTail;
bool                 g_acfDirty = true;    // re-detect next frame: start, plane or livery loaded, settings
float                g_acfSentAt = -1.f;   // PT_AIRCRAFT sent at, waiting for the server's echo
bool                 g_acfAcked = false;

std::string acfModelPath() {
    char file[512] = {0}, path[1024] = {0};
    XPLMGetNthAircraftModel(0, file, path);
    return std::string(path[0] ? path : file);
}

void detectAircraft() {
    g_acfDirty = false;
    const std::string before = g_acf.icao + "/" + g_acf.source;
    g_acf = xr::acftype::resolve(sd(g_ref.acfIcao, 40), acfModelPath(), sd(g_ref.acfDescrip, 260), g_cfg.acIcao, &g_doc8643);
    g_acfTail = trim(sd(g_ref.acfTail, 40));
    if (before != g_acf.icao + "/" + g_acf.source)
        logMsg("aircraft: %s (%s)%s%s%s%s", g_acf.icao.c_str(), g_acf.source.c_str(), g_acf.note.empty() ? "" : " -- ",
               g_acf.note.c_str(), g_acfTail.empty() ? "" : ", tail ", g_acfTail.c_str());
}

// The ICAO type to tell everyone (acftype.h decides: the settings field, the sim, a known add-on, a default).
std::string effectiveIcao() {
    if (g_acf.icao.empty()) detectAircraft();
    return g_acf.icao;
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

// The livery folder name in full (the login carries 15 characters, PT_AIRCRAFT 31).
std::string liveryFull() {
    std::string path = sd(g_ref.acfLivery, 1023);
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1), out;
    for (char c : name) { const unsigned char u = (unsigned char)c; if (u >= 32 && u < 127) out += c; if (out.size() >= 31) break; }
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
    bool        ai = false;     // v6: driven by an ATC program, not a pilot
    bool        drawn = true;   // false: over the AI cap, listed but not in the sky
    // v6: its height above the ground, the one the server routes its radio by
    // (docs/radio.md); unknown from a server that sends 108-byte entries.
    float       aglM = 0.f;
    bool        aglKnown = false;
};

std::map<uint32_t, Remote> g_remote;
int g_rejected = 0;   // traffic entries dropped as implausible

// ---------------------------------------------------------------------------
// v6: air traffic control -- stations online, the ATIS, our flight plan
// ---------------------------------------------------------------------------
struct AtcStation {
    uint32_t    sid = 0;
    std::string callsign, icao;
    uint32_t    freqKhz = 0;
    uint8_t     kind = 0;       // xr::AtcKind
    // where it transmits from and what it covers, as the server routes it
    bool        hasPos = false;
    double      lat = 0, lon = 0;
    float       altMslM = 0.f, antennaM = 0.f;
    uint16_t    rangeNm = 0, ceilingFt = 0;
};
std::vector<AtcStation> g_atcList;          // the last list the server sent
float                   g_atcListAt = -1000.f;
struct AtisText {
    std::string icao, text;
    char        letter = 'A';
    uint32_t    freqKhz = 0;
    float       at = 0.f;
};
std::map<uint32_t, AtisText> g_atis;        // by frequency
// Our flight plan, as far as ATC has taken it.
struct FlightPlanState {
    bool        filed = false;
    float       filedAt = -1000.f;
    uint8_t     status = 0;     // xr::FlightPlanStatus, 0 before any answer
    uint16_t    squawk = 0;
    std::string dep, dest, remarks;
};
FlightPlanState g_fp;
// S3: the pilot's latest SimBrief OFP once imported, and the "press again to overwrite" window for the .fms.
xr::simbrief::Ofp g_ofp;
float g_ofpAt = -1000.f;
int   g_ofpSerial = 0;
float g_fmsConfirmUntil = -1.f;
void applyOfp(const xr::simbrief::Ofp& o);
std::string xplaneRoot() {
    char p[1024] = {0};
    XPLMGetSystemPath(p);
    std::string s = p;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}
// Rows of the ATC list as the draw put them, for the click test: a click
// tunes the station's frequency.
struct AtcRow { int y; uint32_t freqKhz; int radio; std::string callsign; };
std::vector<AtcRow> g_atcRows;

// Defined further down, with the roster and the settings window.
std::string asCallsign(const std::string& raw);
std::string upper(std::string v);

const char* atcKindName(uint8_t k) {
    switch (k) {
        case xr::ATC_DEL:  return "Delivery";
        case xr::ATC_GND:  return "Ground";
        case xr::ATC_TWR:  return "Tower";
        case xr::ATC_APP:  return "Approach";
        case xr::ATC_CTR:  return "Centre";
        case xr::ATC_ATIS: return "ATIS";
        default:           return "ATC";
    }
}

bool isAtcStation(const std::string& callsign) {
    for (const auto& s : g_atcList) if (s.callsign == callsign) return true;
    return false;
}

// Defined with the hosting and settings code further down; the main window
// needs them.
std::string shareAddress();
std::string shareAddressPlain();
void openSettings();
void reconnect();
void setPtt(bool down);
xr::tower::Config towerConfig();
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

// ---------------------------------------------------------------------------
// the radio model (docs/radio.md, radio.h)
// ---------------------------------------------------------------------------
// The flight's air is ducting tonight (the ATC list says so): VHF carries far
// over the sea. Our own height above the ground, as last sent. And the terrain
// on the path to each station, sampled from the scenery a few probes a frame.
bool  g_ducting = false;
float g_myAglM  = 0.f;
xr::shadow::Tracker g_shadow(xr::terrainAt);
// Probes per frame for the path sampler, and the time it may take at most:
// a station's 100 km path is then sampled in three or four frames.
constexpr int    kShadowProbesPerFrame = 32;
constexpr double kShadowMaxMs          = 0.4;
// Over this much water a duct carries the signal (the server relays the whole
// extension; only a path over the sea gets it here).
constexpr double kDuctWetShare = 0.7;

xr::radio::Node aircraftNode(double lat, double lon, double altMslM, double aglM) {
    xr::radio::Node n;
    n.lat = lat; n.lon = lon; n.altMslM = altMslM;
    n.heightFt = xr::radio::aircraftHeightFt(aglM);
    return n;
}

xr::radio::Node remoteNode(const Remote& r) {
    return aircraftNode(r.lat, r.lon, r.altMslM,
                        xr::radio::effectiveAglM(r.aglKnown, r.aglM, r.onGround != 0, r.altMslM));
}

xr::radio::Node stationNode(const AtcStation& s) {
    xr::radio::Node n;
    n.lat = s.lat; n.lon = s.lon; n.altMslM = s.altMslM;
    n.heightFt = (double)s.antennaM * xr::radio::kFtPerM;
    n.atc = true; n.rangeNm = s.rangeNm; n.ceilingFt = s.ceilingFt;
    return n;
}

// One station's signal at our receiver: the model's link, less what the
// terrain on the path costs -- except inside a station's service volume, which
// its own antennas are placed to cover -- with the duct only over the sea.
// Into the voice mixer, which makes the noise, flutter, break-up and the
// capture of one transmission by a stronger one out of it.
void applySignal(uint32_t sid, const xr::radio::Node& tx, const xr::radio::Node& rx, double txAntMslM,
                 double rxAntMslM, uint32_t freqKhz, bool urgent, double now) {
    const double dist = xr::radio::distanceNm(tx.lat, tx.lon, rx.lat, rx.lon);
    xr::radio::Shadow sh;
    bool known = false;
    if (!xr::radio::inServiceVolume(tx, rx) && dist < xr::radio::coverageNm(tx, rx, g_ducting)) {
        g_shadow.want(sid, {rx.lat, rx.lon, rxAntMslM}, {tx.lat, tx.lon, txAntMslM}, freqKhz, urgent, now);
        sh = g_shadow.result(sid, &known);
    }
    const bool duct = g_ducting && known && sh.wetFraction >= kDuctWetShare;
    const bool shadowed = known && !duct;           // a duct carries it round the bulge, over water
    const xr::radio::Link l = xr::radio::link(tx, rx, freqKhz, duct, shadowed ? sh.lossDb : 0.0);
    const double q = shadowed ? std::min(l.quality, sh.cap) : l.quality;
    xr::voice::setSignal(sid, (float)q, (float)l.levelDbm);
}

// ---------------------------------------------------------------------------
// plugin state
// ---------------------------------------------------------------------------
xr::UdpSocket    g_sock;
std::atomic<uint32_t> g_sessionId{0};      // read by the network thread too
std::atomic<uint32_t> g_txFreqKhz{0};      // radio the PTT keys, for voice packets
bool             g_connected    = false;
#ifdef XRADIO_UI_IMGUI
int   g_pktCount = 0;          // packets dispatched this second (the window's connection line)
float g_pktCountAt = 0.f;
int   g_pktRate = 0;
xr::ui2::Model   buildModel();
xr::ui2::Actions makeActions();
#endif
float            g_elapsed      = 0.f;
float            g_lastLogin    = -99.f;
float            g_firstLogin   = -1.f;     // when this connection attempt began
std::string      g_loginProblem;            // why the server will not have us
std::string      g_loginIcao, g_loginLivery;   // what the current session was logged in as
float            g_lastRxTime   = -99.f;
bool             g_pttDown      = false;
float            g_pttDownAt    = -1.f;      // g_elapsed when the key went down

// Real transceivers stop transmitting after about half a minute of continuous
// key-down -- 35 s on most GA radios -- so a jammed microphone switch or a
// PTT taped down cannot block the frequency for everybody. The pilot has to
// release and press again. XRADIO_PTT_TIMEOUT shortens it for the harness.
float pttTimeoutS() {
    static const float v = [] {
        const char* e = getenv("XRADIO_PTT_TIMEOUT");
        const float f = e ? (float)atof(e) : 0.f;
        return (f > 0.1f && f < 600.f) ? f : 35.f;
    }();
    return v;
}
std::string      g_status       = "not connected";
std::vector<std::string> g_chatLog;   // newest last, capped

bool someoneElseIsTheSky() { return g_elapsed - g_lastWeatherRx < 30.f; }

// The one-line text input at the bottom of the main window. Only the main
// thread touches it, so keys are applied as they arrive.
std::string g_chatInput;
bool        g_chatFocus = false;
int         g_chatInputY = 0;          // where the draw put the row, for the click test

// The @ list: who can be addressed, offered while the Say field holds "@" and
// no space yet. g_atPick is the highlighted row; the draw records where it
// put each row and whose callsign is on it, for the click test.
int                      g_atPick = 0;
std::vector<int>         g_atRowsY;
std::vector<std::string> g_atShown;
constexpr int            kAtRows = 6;   // more than this and "keep typing"

// The toolbar at the top of the main window: the things a pilot reaches for
// most, as buttons, so none of them needs a trip to the Plugins menu. The
// draw records where each one landed; the click handler tests against that,
// the same way the @ list works.
enum MainAction { kBtnSettings = 0, kBtnReconnect, kBtnHide, kBtnCopy };
struct MainButton { int x0, x1, y; MainAction action; };
std::vector<MainButton> g_mainButtons;
float g_mainCopyAt = -100.f;            // when the Copy button was last pressed
bool  g_mainCopyOk = false;             // ...and whether the clipboard took it

XPLMWindowID     g_window       = nullptr;
XPLMFlightLoopID g_loop         = nullptr;
XPLMCommandRef   g_cmdPtt       = nullptr;
XPLMMenuID       g_menu         = nullptr;
bool             g_enabled      = false;   // between XPluginEnable and XPluginDisable: the flight loop runs

// ---------------------------------------------------------------------------
// what a click asks for, done from the flight loop
// ---------------------------------------------------------------------------
// The ImGui window acts on a click inside its own draw callback, in the middle of an ImGui frame. Saving the
// settings, restarting the hosted server, reopening the audio devices, reconnecting, entering or leaving the
// tower: none of that may run there -- v0.6.0 crashed X-Plane on Apply that way. A button only queues the
// work; runDeferred() does it at the top of the next flight loop, outside every window's frame, and an
// exception thrown by the work goes to Log.txt instead of taking the sim down with it.
struct Deferred { const char* what; std::function<void()> fn; };
std::vector<Deferred> g_deferred;          // main thread only

void defer(const char* what, std::function<void()> fn) {
    g_deferred.push_back({what, std::move(fn)});
    // without a socket the loop idles at 2 Hz; the work is due on the next frame
    if (g_loop && g_enabled) XPLMScheduleFlightLoop(g_loop, -1.0f, 1);
}

void runDeferred() {
    // work that queues more (Apply asks for a reconnect) gets it done in the same pass
    for (int pass = 0; pass < 4 && !g_deferred.empty(); ++pass) {
        std::vector<Deferred> jobs;
        jobs.swap(g_deferred);
        for (Deferred& j : jobs) {
            try {
                j.fn();
            } catch (const std::exception& e) {
                logMsg("%s failed: %s", j.what, e.what());
            } catch (...) {
                logMsg("%s failed: unknown exception", j.what);
            }
        }
    }
}

// The ImGui window's Apply: the copy of the settings it holds, saved by the flight loop (runApply, with the
// settings window below). Two presses before the loop runs save the later one, once.
Settings g_applyWanted;
bool     g_applyQueued = false;
void runApply();
void requestApply(const Settings& s) {
    g_applyWanted = s;
    if (g_applyQueued) return;
    g_applyQueued = true;
    defer("apply", [] { g_applyQueued = false; runApply(); });
}
void pollHosting();          // with hosting, below

// The window's structured log (Phase U): every chat line, with who / what / where when known.
struct Msg { float t = 0; std::string from, text; uint32_t khz = 0; bool atc = false, priv = false, mine = false, guard = false, system = true; };
std::vector<Msg> g_msgLog;
void addChat(const std::string& s) {
    g_chatLog.push_back(s);
    if (g_chatLog.size() > 12) g_chatLog.erase(g_chatLog.begin());
    Msg m; m.t = g_elapsed; m.text = s;
    g_msgLog.push_back(m);
    if (g_msgLog.size() > 200) g_msgLog.erase(g_msgLog.begin());
}
// Fills in the last line's sender and kind (handleText / sendText call this right after addChat).
void tagLastMsg(const std::string& from, const std::string& text, uint32_t khz, bool atc, bool priv, bool mine) {
    if (g_msgLog.empty()) return;
    Msg& m = g_msgLog.back();
    m.from = from; m.text = text; m.khz = khz; m.atc = atc; m.priv = priv; m.mine = mine; m.guard = xr::isGuard(khz); m.system = false;
}

// ---------------------------------------------------------------------------
// notices: what a pilot still sees with the XRadio window closed
// ---------------------------------------------------------------------------
// Most pilots fly with the window shut -- it is a big grey rectangle over the
// scenery. Shut, though, it used to mean a call on the radio left no trace at
// all: nothing in the cockpit, nothing on screen, and the radio log only
// visible to somebody who happened to open the window afterwards. These are
// the few seconds of text that fix that, in the corner X-Plane puts its own
// messages in, and they appear only while the window is closed.
struct Notice {
    std::string text;
    float       at = 0.f;
    int         colour = 0;          // 0 white, 1 amber, 2 green
};
std::vector<Notice> g_notices;
static const size_t kMaxNotices  = 5;

// Ten seconds is long enough to read a line and short enough not to become
// clutter. The tests would otherwise have to sit through it for real, so it
// is readable from the environment -- the same seam the update check uses.
float noticeHoldS() {
    static const float v = [] {
        const char* e = getenv("XRADIO_NOTICE_HOLD");
        const float f = e ? (float)atof(e) : 0.f;
        return (f > 0.1f && f < 120.f) ? f : 10.f;
    }();
    return v;
}

void updateNotices();          // defined with the window, below

void pushNotice(const std::string& text, int colour) {
#ifdef XRADIO_UI_IMGUI
    if (!text.empty()) xr::ui2::host::toast(text, colour == 1 ? 1 : colour == 2 ? 2 : 0);
#endif
    if (!g_cfg.popUps || text.empty()) return;
    g_notices.push_back({text, g_elapsed, colour});
    if (g_notices.size() > kMaxNotices) g_notices.erase(g_notices.begin());
}

// Fetch the latest OFP (official API, only when the pilot asks); the result lands in flightLoop().
void importSimbrief() {
    if (xr::simbrief::busy()) return;
    if (g_cfg.simbriefUser.empty()) { pushNotice("SimBrief: enter your username or pilot ID in Settings first", 1); return; }
    xr::simbrief::fetchAsync(g_cfg.simbriefUser);
    pushNotice("Fetching your latest OFP from SimBrief...", 0);
}
// Write the OFP route as Output/FMS plans/<DEP><DEST>.fms; an existing file is only replaced on a second press.
std::string writeOfpFms() {
    if (!g_ofp.ok) return "import an OFP first";
    std::string path;
    const std::string note = xr::simbrief::writeFms(g_ofp, xplaneRoot(), g_elapsed < g_fmsConfirmUntil, &path);
    g_fmsConfirmUntil = note.rfind("exists", 0) == 0 ? g_elapsed + 20.f : -1.f;
    return note;
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
    //
    // Only a sim that can actually read its weather makes the offer. An
    // X-Plane 11 pilot has nothing to send, and if the server awarded the
    // claim to them, every X-Plane 12 pilot in the flight would be refused
    // and nobody would get a sky -- in a mixed flight, with the setting
    // sitting there saying everyone was sharing.
    p.flags = (g_cfg.shareWeather && xr::weather::available()) ? xr::LF_WEATHER_SOURCE : 0;
    memcpy(buf + off, &p, sizeof(p));
    g_sock.send(buf, off + (int)sizeof(p));
    g_lastLogin = g_elapsed;
    if (g_firstLogin < 0.f) g_firstLogin = g_elapsed;
    if (g_loginProblem.empty()) g_status = "logging in to " + g_sock.endpoint() + "...";
}

// v6: the aircraft just loaded, without logging in again. A server that predates
// PT_AIRCRAFT never echoes it; then the flight loop logs in again with the new type.
void sendAircraft() {
    if (!g_connected) return;
    uint8_t buf[sizeof(xr::Header) + sizeof(xr::AircraftPayload)];
    int off = writeHeader(buf, xr::PT_AIRCRAFT, sizeof(xr::AircraftPayload));
    xr::AircraftPayload a{};
    const std::string airline = xr::acftype::airlineOf(g_cfg.callsign), livery = liveryFull();
    strncpy(a.acIcao,  g_acf.icao.c_str(), sizeof(a.acIcao) - 1);
    strncpy(a.tailnum, g_acfTail.c_str(),  sizeof(a.tailnum) - 1);
    strncpy(a.airline, airline.c_str(),    sizeof(a.airline) - 1);
    strncpy(a.livery,  livery.c_str(),     sizeof(a.livery) - 1);
    a.wingspanDm = (uint16_t)std::lround(g_acf.spanM * 10.f);
    a.lengthDm   = (uint16_t)std::lround(g_acf.lengthM * 10.f);
    a.mtowKg     = (uint32_t)g_acf.mtowKg;
    a.wtc        = g_acf.wtc;
    a.source     = g_acf.source == "manual" ? xr::AS_MANUAL : g_acf.source == "mapped" ? xr::AS_MAPPED
                 : g_acf.source == "detected" ? xr::AS_DETECTED : xr::AS_DEFAULT;
    memcpy(buf + off, &a, sizeof(a));
    g_sock.send(buf, off + (int)sizeof(a));
    g_acfSentAt = g_elapsed;
    g_acfAcked = false;
}

// PART D: the pilot's own TCAS, for TowerTrainer's AI pilots and the controller. Sent when it changes and every
// second while an advisory is up. The RA's intruder is named when it is one of our aircraft (XPMP2 mode S ids).
xr::tcas::State g_tcas;
float           g_tcasSentAt = -100.f;
void tcasTick() {
    if (!g_ref.tcasAdvisory) return;
    const int adv = XPLMGetDatai(g_ref.tcasAdvisory);
    const int msg = g_ref.tcasMessage ? XPLMGetDatai(g_ref.tcasMessage) : 0;
    float bands[6] = {0, 0, 0, 0, 0, 0};
    if (g_ref.tcasBands) XPLMGetDatavf(g_ref.tcasBands, bands, 0, 6);
    int threat[64] = {0};
    const int n = g_ref.tcasThreat ? XPLMGetDatavi(g_ref.tcasThreat, threat, 0, 64) : 0;
    const xr::tcas::State s = xr::tcas::read(adv, msg, bands, threat, n, g_tcas.level, g_tcas.sense);
    uint32_t intruder = 0;
    std::string who;
    if (s.threatIndex >= 0 && g_ref.tcasModeS) {
        int id = 0;
        if (XPLMGetDatavi(g_ref.tcasModeS, &id, s.threatIndex, 1) == 1)
            for (const auto& kv : g_remote)
                if ((0xF00000u | (kv.first & 0x0FFFFFu)) == (uint32_t)id) { intruder = kv.first; who = kv.second.callsign; }
    }
    const bool changed = s != g_tcas;
    if (s.level == xr::TCAS_RA && g_tcas.level != xr::TCAS_RA)
        pushNotice(std::string("TCAS RA") + (s.sense > 0 ? " CLIMB" : s.sense < 0 ? " DESCEND" : "") + (who.empty() ? "" : ": " + who), 3);
    g_tcas = s;
    if (!g_connected || !(changed || ((s.level == xr::TCAS_TA || s.level == xr::TCAS_RA) && g_elapsed - g_tcasSentAt > 1.f))) return;
    g_tcasSentAt = g_elapsed;
    uint8_t buf[sizeof(xr::Header) + sizeof(xr::TcasPayload)];
    int off = writeHeader(buf, xr::PT_TCAS, sizeof(xr::TcasPayload));
    xr::TcasPayload t{};
    t.timeMs = nowMs(); t.level = s.level; t.sense = s.sense; t.message = s.message;
    t.vsTargetMs = s.vsTargetMs; t.vsMs = fd(g_ref.vh); t.intruderSid = intruder;
    memcpy(buf + off, &t, sizeof(t));
    g_sock.send(buf, off + (int)sizeof(t));
}

void sendPosition() {
    xr::PositionPayload p{};
    p.lat         = dd(g_ref.lat);
    p.lon         = dd(g_ref.lon);
    // Altitude is the one field the receiver cannot simply draw as sent.
    // `elevation` is where our *datum point* is, which on the ground is a
    // metre or three up in the air, on top of the gear. The receiver hands it
    // to XPMP2, which adds the CSL model's own VERT_OFFSET to stand it on its
    // wheels -- so the gear height would be counted twice and the model
    // hovers. What the receiver needs while we are on the ground is the
    // ground itself, so each model's own offset can put its own wheels on it.
    //
    // Ask the sim where that is. The first version of this subtracted y_agl
    // from the datum altitude, which is the same thing only if y_agl is
    // measured to the datum -- and on some aircraft it is not, so the
    // subtraction took off nothing and the model went on hovering. The probe
    // has no such assumption in it. y_agl stays as the fallback for the rare
    // frame where the probe misses.
    const bool onGnd = id(g_ref.onGround) != 0;
    double     altM  = dd(g_ref.elev);
    if (onGnd) {
        double ground = 0.0;
        if (xr::groundElevM(p.lat, p.lon, altM, &ground) &&
            ground <= altM + 1.0 &&                       // the ground is below us
            ground >= altM - xr::kMaxDatumAboveGroundM) { // and not absurdly so
            altM = ground;
        } else {
            float gearUp = fd(g_ref.yAgl);
            if (!(gearUp > 0.f) || gearUp > 12.f) gearUp = 0.f;   // also catches NaN
            altM -= gearUp;
        }
    }
    p.altMslM     = (float)altM;
    p.headingTrue = fd(g_ref.psi);
    p.pitch       = fd(g_ref.theta);
    p.roll        = fd(g_ref.phi);
    p.gsMs        = fd(g_ref.gs);
    p.gearRatio   = firstOfArray(g_ref.gear);
    p.flapRatio   = fd(g_ref.flap);
    // A dead radio is on no frequency at all, so the server does not route
    // anyone's voice to us and other pilots do not see us listening.
    p.com1Khz     = comPowered(1) ? comKhz(1) : 0u;
    p.com2Khz     = comPowered(2) ? comKhz(2) : 0u;
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

    uint8_t tx = xr::TX_NONE;
    if (g_pttDown) tx = (txCom() == 2) ? xr::TX_COM2 : xr::TX_COM1;
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
    // v6, the radio model: our height above the ground -- the sim's own, the
    // centre of gravity over the terrain or the water; on the ground 0. The
    // server works the VHF horizon out from it (docs/radio.md): from the
    // altitude, two aircraft on Innsbruck's apron at 1900 ft would talk over
    // a hundred miles.
    float agl = onGnd ? 0.f : fd(g_ref.yAgl);
    if (!std::isfinite(agl) || agl < 0.f) agl = 0.f;
    if (agl > 30000.f) agl = 30000.f;
    p.aglM = agl;
    p.svcRangeNm = 0;          // an aircraft has no service volume
    p.svcCeilingFt = 0;
    g_myAglM = agl;

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
    th.freqKhz     = to.empty() ? comKhz(txCom()) : 0u;
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
    if (!std::isfinite(e.aglM) || e.aglM < -1000.f || e.aglM > 40000.f) return false;
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
    // v6 entries end with the aircraft's height above the ground; a server
    // from before that sends them 108 bytes long, and then it is not known.
    const bool v6 = len - off >= (int)th.count * (int)sizeof(xr::TrafficEntry);
    const int esize = v6 ? (int)sizeof(xr::TrafficEntry) : xr::kTrafficEntrySizeV5;
    for (int i = 0; i < th.count; ++i) {
        if (off + esize > len) break;
        xr::TrafficEntry e{};
        memcpy(&e, payload + off, (size_t)esize);
        off += esize;

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
        r.ai        = xr::isAiSid(e.sessionId);
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
        r.aglM      = e.aglM;
        r.aglKnown  = v6;
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
        // Computer-flown aircraft over the cap are kept in the list but not
        // in the sky; the cap is applied below, nearest first, once the
        // whole packet is in.
        if (r.ai && !r.drawn) continue;
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

    // v6: the cap on computer-flown aircraft, nearest first. A busy airport
    // can mean sixty of them; the CSL budget belongs to the nearest.
    {
        std::vector<Remote*> ai;
        for (auto& kv : g_remote) if (kv.second.ai) ai.push_back(&kv.second);
        const double myLat = dd(g_ref.lat), myLon = dd(g_ref.lon);
        std::sort(ai.begin(), ai.end(), [&](const Remote* a, const Remote* b) {
            return distanceNm(myLat, myLon, a->lat, a->lon) < distanceNm(myLat, myLon, b->lat, b->lon);
        });
        const int cap = (int)(g_cfg.aiMax + 0.5f);
        for (size_t i = 0; i < ai.size(); ++i) {
            const bool draw = (int)i < cap;
            if (ai[i]->drawn && !draw) xr::csl::remove(ai[i]->sid);
            ai[i]->drawn = draw;
        }
    }
}

// ---------------------------------------------------------------------------
// v6: the ATC list, the ATIS, flight plans
// ---------------------------------------------------------------------------
void handleAtcList(const uint8_t* payload, int len) {
    if (len < (int)sizeof(xr::AtcListHeader)) return;
    xr::AtcListHeader lh{};
    memcpy(&lh, payload, sizeof(lh));
    if (lh.count > xr::kMaxAtcEntries) return;
    if (len != (int)(sizeof(lh) + lh.count * sizeof(xr::AtcEntry))) return;

    std::vector<AtcStation> list;
    int off = (int)sizeof(lh);
    for (int i = 0; i < lh.count; ++i) {
        xr::AtcEntry e{};
        memcpy(&e, payload + off, sizeof(e));
        off += (int)sizeof(e);
        AtcStation s;
        s.sid      = e.sessionId;
        s.callsign = asCallsign(sanitizeText(e.callsign, sizeof(e.callsign)));
        s.icao     = sanitizeText(e.icao, sizeof(e.icao));
        s.freqKhz  = e.freqKhz;
        s.kind     = e.kind;
        // where it transmits from and what it covers -- checked like a traffic
        // entry, since it decides what the radio sounds like
        s.hasPos   = (e.flags & xr::AE_POSITION) && std::isfinite(e.lat) && std::isfinite(e.lon) &&
                     e.lat >= -90.0 && e.lat <= 90.0 && e.lon >= -180.0 && e.lon <= 180.0 &&
                     std::isfinite(e.altMslM) && e.altMslM > -1000.f && e.altMslM < 40000.f &&
                     std::isfinite(e.antennaM) && e.antennaM >= 0.f && e.antennaM < 40000.f;
        if (s.hasPos) {
            s.lat = e.lat; s.lon = e.lon; s.altMslM = e.altMslM; s.antennaM = e.antennaM;
            s.rangeNm = e.rangeNm; s.ceilingFt = e.ceilingFt;
        }
        if (!s.callsign.empty()) list.push_back(s);
    }
    // The flight's air: when it starts carrying VHF far over the sea, and when
    // it stops, the radio log says so -- otherwise a station from across the
    // gulf would come in for no reason a pilot could see.
    const bool duct = (lh.flags & xr::AL_DUCTING) != 0;
    if (duct != g_ducting) {
        g_ducting = duct;
        const char* line = duct
            ? "Tropospheric ducting tonight: VHF carries far past the horizon over the sea"
            : "The duct has gone: VHF reaches as far as the horizon again";
        addChat(line);
        pushNotice(line, duct ? 1 : 0);
    }
    // A station that was not in the last list is news worth a line: the
    // pilot may have been waiting for exactly that controller.
    for (const auto& s : list) {
        if (s.kind == xr::ATC_ATIS) continue;
        bool known = false;
        for (const auto& o : g_atcList) known = known || o.sid == s.sid;
        if (!known && g_atcListAt > 0.f) {
            char line[160];
            snprintf(line, sizeof(line), "%s (%s) is online on %s -- click it in the XRadio window to tune",
                     s.callsign.c_str(), atcKindName(s.kind), freqLabel(s.freqKhz).c_str());
            addChat(line);
            pushNotice(line, 2);
        }
    }
    g_atcList = list;
    g_atcListAt = g_elapsed;
}

void handleAtis(const uint8_t* payload, int len) {
    if (len < (int)sizeof(xr::AtisPayload)) return;
    xr::AtisPayload ah{};
    memcpy(&ah, payload, sizeof(ah));
    int textLen = std::min<int>(ah.textLen, len - (int)sizeof(ah));
    textLen = std::min(textLen, xr::kMaxAtisBytes);
    if (textLen < 0 || ah.freqKhz == 0) return;
    AtisText& a = g_atis[ah.freqKhz];
    const bool fresh = a.letter != ah.letter || a.icao.empty();
    a.icao    = sanitizeText(ah.icao, sizeof(ah.icao));
    a.letter  = (ah.letter >= 'A' && ah.letter <= 'Z') ? ah.letter : 'A';
    a.text    = sanitizeText(reinterpret_cast<const char*>(payload + sizeof(ah)), (size_t)textLen);
    a.freqKhz = ah.freqKhz;
    a.at      = g_elapsed;
    if (fresh) {
        char line[120];
        snprintf(line, sizeof(line), "ATIS %s information %c on %s", a.icao.c_str(), a.letter,
                 freqLabel(a.freqKhz).c_str());
        addChat(line);
        pushNotice(line, 0);
    }
}

void handleFlightPlan(const uint8_t* payload, int len) {
    if (len < (int)sizeof(xr::FlightPlanPayload)) return;
    xr::FlightPlanPayload fp{};
    memcpy(&fp, payload, sizeof(fp));
    if (fp.sessionId != g_sessionId.load()) return;      // another pilot's plan: not for us
    g_fp.status  = fp.status;
    g_fp.squawk  = xr::validSquawk(fp.squawk) ? fp.squawk : 0;
    g_fp.remarks = sanitizeText(fp.remarks, sizeof(fp.remarks));
    g_fp.dep     = sanitizeText(fp.dep, sizeof(fp.dep));
    g_fp.dest    = sanitizeText(fp.dest, sizeof(fp.dest));
    char line[240];
    if (fp.status == xr::FP_CANCELLED) {
        snprintf(line, sizeof(line), "ATC cancelled your flight plan%s%s",
                 g_fp.remarks.empty() ? "" : ": ", g_fp.remarks.c_str());
        g_fp.filed = false;
    } else if (fp.status == xr::FP_CLEARED) {
        snprintf(line, sizeof(line), "Clearance: %s%s%s", g_fp.remarks.c_str(),
                 g_fp.squawk ? "  -- squawk " : "", g_fp.squawk ? std::to_string(g_fp.squawk).c_str() : "");
    } else {
        snprintf(line, sizeof(line), "ATC has your flight plan %s -> %s%s%04u%s",
                 g_fp.dep.c_str(), g_fp.dest.c_str(),
                 g_fp.squawk ? ", squawk " : "", (unsigned)g_fp.squawk,
                 g_fp.squawk ? " -- set it on the transponder" : "");
    }
    addChat(line);
    pushNotice(line, 1);
    if (g_cfg.atcSoundCue) XPLMSpeakString(line);
}

// File the plan on the ATC tab. Returns what went wrong, or "".
std::string sendFlightPlan() {
    if (!g_connected) return "not connected";
    std::string dep = upper(trim(g_cfg.fpDep)), dest = upper(trim(g_cfg.fpDest));
    if (dep.size() < 3 || dest.size() < 3) return "departure and destination are needed (ICAO codes)";
    xr::FlightPlanPayload fp{};
    fp.sessionId = 0;
    strncpy(fp.callsign, g_cfg.callsign.c_str(), sizeof(fp.callsign) - 1);
    strncpy(fp.acIcao, effectiveIcao().c_str(), sizeof(fp.acIcao) - 1);
    const std::string rules = upper(trim(g_cfg.fpRules));
    fp.rules  = (uint8_t)(rules.empty() ? 'V' : rules[0]);
    if (fp.rules != 'I' && fp.rules != 'V' && fp.rules != 'Y' && fp.rules != 'Z') fp.rules = 'V';
    fp.status = xr::FP_FILED;
    strncpy(fp.dep,  dep.c_str(),  sizeof(fp.dep)  - 1);
    strncpy(fp.dest, dest.c_str(), sizeof(fp.dest) - 1);
    strncpy(fp.altn, upper(trim(g_cfg.fpAltn)).c_str(), sizeof(fp.altn) - 1);
    fp.cruiseFt = (uint32_t)atoi(g_cfg.fpCruise.c_str());
    strncpy(fp.route,   trim(g_cfg.fpRoute).c_str(),   sizeof(fp.route)   - 1);
    strncpy(fp.remarks, trim(g_cfg.fpRemarks).c_str(), sizeof(fp.remarks) - 1);
    uint8_t buf[sizeof(xr::Header) + sizeof(fp)];
    int off = writeHeader(buf, xr::PT_FLIGHTPLAN, (uint16_t)sizeof(fp));
    memcpy(buf + off, &fp, sizeof(fp));
    g_sock.send(buf, off + (int)sizeof(fp));
    g_fp = FlightPlanState{};
    g_fp.filed = true; g_fp.filedAt = g_elapsed; g_fp.dep = dep; g_fp.dest = dest;
    char line[120];
    snprintf(line, sizeof(line), "Flight plan filed: %s -> %s %c", dep.c_str(), dest.c_str(), (char)fp.rules);
    addChat(line);
    return "";
}

void cancelFlightPlan() {
    if (!g_connected || !g_fp.filed) return;
    xr::FlightPlanPayload fp{};
    strncpy(fp.callsign, g_cfg.callsign.c_str(), sizeof(fp.callsign) - 1);
    fp.status = xr::FP_CANCELLED;
    uint8_t buf[sizeof(xr::Header) + sizeof(fp)];
    int off = writeHeader(buf, xr::PT_FLIGHTPLAN, (uint16_t)sizeof(fp));
    memcpy(buf + off, &fp, sizeof(fp));
    g_sock.send(buf, off + (int)sizeof(fp));
    g_fp = FlightPlanState{};
    addChat("Flight plan cancelled");
}

// Tune a radio from the ATC list: COM1 for a station, COM2 for an ATIS. The
// 8.33 kHz dataref takes kHz; the older 25 kHz one takes units of 10 kHz.
void tuneRadio(int which, uint32_t khz) {
    if (khz == 0) return;
    XPLMDataRef fine   = (which == 2) ? g_ref.com2   : g_ref.com1;
    XPLMDataRef coarse = (which == 2) ? g_ref.com2Hz : g_ref.com1Hz;
    if (fine)        XPLMSetDatai(fine, (int)khz);
    else if (coarse) XPLMSetDatai(coarse, (int)(khz / 10u));
    else return;
    char line[96];
    snprintf(line, sizeof(line), "COM%d tuned to %s", which, freqLabel(khz).c_str());
    addChat(line);
    if (g_connected) sendPosition();      // the server learns the new frequency at once
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
    const bool fromAtc = isAtcStation(from);
    if (!to.empty()) {
        snprintf(line, sizeof(line), "[direct] %s: %s", from.c_str(), msg.c_str());
    } else if (fromAtc) {
        snprintf(line, sizeof(line), "[%s] ATC %s: %s", freqLabel(th.freqKhz).c_str(),
                 from.c_str(), msg.c_str());
    } else {
        snprintf(line, sizeof(line), "[%s] %s: %s", freqLabel(th.freqKhz).c_str(),
                 from.c_str(), msg.c_str());
    }
    addChat(line);
    tagLastMsg(from, msg, th.freqKhz, fromAtc, !to.empty(), false);
    // Amber for the two that were aimed at getting your attention, green for
    // a controller, white for ordinary chatter on a frequency you are already
    // listening to.
    pushNotice(line, (!to.empty() || xr::isGuard(th.freqKhz)) ? 1 : (fromAtc ? 2 : 0));
    // v6: a call from ATC, or a message meant only for you, is read out so it
    // is not missed with the window shut -- the sim's own voice, which every
    // pilot already knows as the one that says "autopilot disengaged".
    if (g_cfg.atcSoundCue && (fromAtc || !to.empty())) {
        char cue[300];
        snprintf(cue, sizeof(cue), "%s%s: %s", fromAtc ? "" : "Message from ", from.c_str(), msg.c_str());
        XPLMSpeakString(cue);
    }
}

// ---------------------------------------------------------------------------
// the roster: everyone in the flight, for the @ list
// ---------------------------------------------------------------------------
struct RosterPilot {
    uint32_t    sid = 0;
    std::string callsign, acIcao;
};
std::vector<RosterPilot> g_roster;              // the last complete roster
float                    g_rosterAt = -1000.f;  // g_elapsed when it completed
// A roster too big for one packet arrives in parts; this is the one being put
// together. See RosterHeader in protocol.h.
uint16_t                              g_rosterEpoch = 0;
std::vector<std::vector<RosterPilot>> g_rosterParts;
std::vector<bool>                     g_rosterHave;

// The server re-sends the roster every five seconds. One that has gone quiet
// for three times that is a server that does not send one at all -- an older
// build -- and then the @ list offers the aircraft in the traffic list.
constexpr float kRosterFreshS = 15.f;
bool rosterFresh() { return g_elapsed - g_rosterAt < kRosterFreshS; }

// What comes after "@" has to be something splitDirect() will accept, so a
// name that could not be typed back is not offered.
std::string asCallsign(const std::string& raw) {
    if (raw.empty() || raw.size() > 15) return "";
    std::string out;
    for (char c : raw) {
        const unsigned char u = (unsigned char)c;
        if (!isalnum(u) && c != '-' && c != '_') return "";
        out += (char)toupper(u);
    }
    return out;
}

void handleRoster(const uint8_t* payload, int len) {
    if (len < (int)sizeof(xr::RosterHeader)) return;
    xr::RosterHeader rh{};
    memcpy(&rh, payload, sizeof(rh));
    // Sixty pilots is two packets. Anything claiming far more is not a
    // roster, and a hostile server does not get to make us allocate for it.
    if (rh.parts == 0 || rh.parts > 8 || rh.part >= rh.parts) return;
    if (rh.count > xr::kMaxRosterEntries) return;
    if (len != (int)(sizeof(rh) + rh.count * sizeof(xr::RosterEntry))) return;

    std::vector<RosterPilot> part;
    int off = (int)sizeof(rh);
    for (int i = 0; i < rh.count; ++i) {
        xr::RosterEntry e{};
        memcpy(&e, payload + off, sizeof(e));
        off += (int)sizeof(e);
        RosterPilot rp;
        rp.sid      = e.sessionId;
        rp.callsign = asCallsign(sanitizeText(e.callsign, sizeof(e.callsign)));
        rp.acIcao   = sanitizeText(e.acIcao, sizeof(e.acIcao));
        if (rp.sid != 0 && !rp.callsign.empty()) part.push_back(rp);
    }

    if (rh.epoch != g_rosterEpoch || g_rosterParts.size() != rh.parts) {
        g_rosterEpoch = rh.epoch;
        g_rosterParts.assign(rh.parts, std::vector<RosterPilot>());
        g_rosterHave.assign(rh.parts, false);
    }
    g_rosterParts[rh.part] = std::move(part);
    g_rosterHave[rh.part] = true;
    for (bool have : g_rosterHave) if (!have) return;

    g_roster.clear();
    for (const auto& p : g_rosterParts)
        for (const auto& e : p)
            if (g_roster.size() < 60) g_roster.push_back(e);
    g_rosterAt = g_elapsed;
}

struct AtCandidate {
    std::string callsign, acIcao;
    double      distNm = -1.0;     // -1: not in our traffic, so not known
};

// Who "@" can reach, best first. The roster is everyone in the flight, which
// is who a direct message can actually reach; without one the list is the
// aircraft we can see. Never ourselves. Names that start with what has been
// typed come before names that only contain it; among those, the aircraft we
// can see, nearest first, and then the rest alphabetically.
std::vector<AtCandidate> atCandidates(const std::string& typed) {
    std::string want;
    for (char c : typed) want += (char)toupper((unsigned char)c);
    const std::string me = asCallsign(g_cfg.callsign);
    const uint32_t mySid = g_sessionId.load();
    const double myLat = dd(g_ref.lat), myLon = dd(g_ref.lon);
    auto distTo = [&](uint32_t sid) {
        auto it = g_remote.find(sid);
        return it == g_remote.end() ? -1.0
                                    : distanceNm(myLat, myLon, it->second.lat, it->second.lon);
    };

    std::vector<AtCandidate> all;
    if (rosterFresh()) {
        for (const auto& p : g_roster) {
            if (p.sid == mySid || p.callsign == me) continue;
            all.push_back({p.callsign, p.acIcao, distTo(p.sid)});
        }
    } else {
        for (const auto& kv : g_remote) {
            const std::string cs = asCallsign(kv.second.callsign);
            if (cs.empty() || cs == me) continue;
            all.push_back({cs, kv.second.acIcao, distTo(kv.first)});
        }
    }

    std::vector<AtCandidate> out;
    for (const auto& c : all) {
        if (c.callsign.find(want) == std::string::npos) continue;
        bool dup = false;                         // one callsign, two sessions
        for (const auto& o : out) dup = dup || o.callsign == c.callsign;
        if (!dup) out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [&](const AtCandidate& a, const AtCandidate& b) {
        const bool pa = a.callsign.compare(0, want.size(), want) == 0;
        const bool pb = b.callsign.compare(0, want.size(), want) == 0;
        if (pa != pb) return pa;
        const bool ka = a.distNm >= 0, kb = b.distNm >= 0;
        if (ka != kb) return ka;
        if (ka && a.distNm != b.distNm) return a.distNm < b.distNm;
        return a.callsign < b.callsign;
    });
    return out;
}

// The list is up while a callsign is still being chosen: the Say field has
// the keyboard, starts with "@", and has no space in it yet.
bool atActive() {
    return g_chatFocus && !g_chatInput.empty() && g_chatInput[0] == '@' &&
           g_chatInput.find(' ') == std::string::npos;
}

void atComplete(const std::string& callsign) {
    g_chatInput = "@" + callsign + " ";
    g_atPick = 0;
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
float                             g_netErrAt = -1000.f;   // g_elapsed when it last failed to open
std::string                       g_netErrLogged;         // so Log.txt gets each error once
// A host name that would not resolve usually resolves a little later -- the
// sim was started before the network was up, or a VPN was still connecting --
// so a failed open is tried again this often, rather than left until the
// pilot notices and finds the Reconnect menu item.
constexpr float                   kSocketRetryS = 15.f;
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
    g_roster.clear();                  // another server, another flight
    g_rosterAt = -1000.f;
    g_rosterParts.clear();
    g_rosterHave.clear();
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
#ifdef XRADIO_UI_IMGUI
    if (g_elapsed - g_pktCountAt >= 1.f) { g_pktRate = g_pktCount; g_pktCount = 0; g_pktCountAt = g_elapsed; }
#endif
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
#ifdef XRADIO_UI_IMGUI
        ++g_pktCount;
#endif

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
                sendAircraft();   // tail number, airline, size and weight for the ATC stations
                break;
            }
            case xr::PT_AIRCRAFT: {
                // the server's receipt: it flies the new type in every traffic packet from now on
                if (h.sessionId == g_sessionId.load()) {
                    g_acfAcked = true;
                    g_loginIcao = g_acf.icao;
                    g_loginLivery = effectiveLivery();
                }
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
                    char why[160];
                    snprintf(why, sizeof(why),
                             "the server runs another XRadio version (this is v%s, protocol %u) -- everyone must be on the same one",
                             xr::brand::version(), (unsigned)xr::kProtoVersion);
                    g_loginProblem = why;
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
            case xr::PT_ROSTER:  handleRoster(payload, h.payloadLen);  break;
            case xr::PT_ATC_LIST:   handleAtcList(payload, h.payloadLen);    break;
            case xr::PT_ATIS:       handleAtis(payload, h.payloadLen);       break;
            case xr::PT_FLIGHTPLAN: handleFlightPlan(payload, h.payloadLen); break;
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
    runDeferred();              // what the windows' buttons asked for, outside their frames
    pollHosting();              // a hosted server the worker has (re)started or stopped
    xr::tower::tick(elapsedSinceLast, g_elapsed);
    {   // S3: a finished SimBrief fetch lands here, on the main thread
        xr::simbrief::Ofp o;
        if (xr::simbrief::poll(&o)) {
            if (o.ok) {
                g_ofp = o; g_ofpAt = g_elapsed; ++g_ofpSerial;
                applyOfp(o);
                pushNotice("SimBrief OFP: " + o.summary(), 2);
                addChat(("SimBrief OFP imported: " + o.summary() + " -- check the flight plan and press File").c_str());
                if (!o.callsign.empty() && upper(trim(g_cfg.callsign)) != upper(trim(o.callsign)))
                    pushNotice("The OFP is for " + o.callsign + ", you fly as " + g_cfg.callsign + " -- the plan is filed under your callsign", 1);
            } else {
                pushNotice(o.error, 3);
                addChat(o.error.c_str());
            }
        }
    }
#ifdef XRADIO_UI_IMGUI
    xr::ui2::host::update(buildModel());
    xr::ui2::host::updateTower(xr::tower::state(), xr::tower::active(), xr::tower::windowsVisible(), g_cfg.towerCode, g_cfg.towerPos);
#endif

    if (!g_sockReady.load()) {
        // Not open yet: either still resolving, or it failed and the thread
        // has left the reason for us.
        bool failed = false;
        {
            std::lock_guard<std::mutex> lk(g_netErrMx);
            failed = !g_netErr.empty();
            if (failed && g_status.compare(0, 12, "socket error") != 0) {
                g_status = "socket error: " + g_netErr;
                if (g_netErr != g_netErrLogged) {       // once per error, not per retry
                    logMsg("%s", g_status.c_str());
                    g_netErrLogged = g_netErr;
                }
                g_netErrAt = g_elapsed;
            }
        }
        if (failed && g_elapsed - g_netErrAt > kSocketRetryS) {
            netStop();
            netStart();
        }
        return 0.5f;
    }

    pumpNetwork();
    xr::voice::tick();
    if (g_acfDirty) {                       // a plane or livery was loaded (XPluginReceiveMessage)
        detectAircraft();
        if (g_connected && (g_acf.icao != g_loginIcao || effectiveLivery() != g_loginLivery)) {
            pushNotice("Aircraft: " + g_acf.icao + " (" + g_acf.source + ")", 2);
            sendAircraft();
        }
    }
    tcasTick();

    // Somebody keying up is the other half of "I cannot see the window": you
    // can hear them, but not who they are or where they are calling from.
    // One notice per transmission, when they start -- not per frame.
    {
        static std::vector<uint32_t> wasSpeaking;
        const std::vector<uint32_t> now = xr::voice::activeSpeakers();
        for (uint32_t sid : now) {
            if (std::find(wasSpeaking.begin(), wasSpeaking.end(), sid) != wasSpeaking.end())
                continue;
            auto it = g_remote.find(sid);
            const std::string who = it != g_remote.end() ? it->second.callsign
                                                         : std::to_string(sid);
            const uint32_t f = xr::voice::speakerFreq(sid);
            char line[160];
            if (f) snprintf(line, sizeof(line), "%s is calling on %s", who.c_str(),
                            freqLabel(f).c_str());
            else   snprintf(line, sizeof(line), "%s is calling", who.c_str());
            pushNotice(line, xr::isGuard(f) ? 1 : 2);
        }
        wasSpeaking = now;
    }
    updateNotices();

    // The radio model (docs/radio.md): every station's signal at our
    // receiver, so a near one is clean, a far one noisy and one behind a
    // mountain weak or gone -- and two at once fight it out in the mixer by
    // level. The path sampler then spends its few probes for the frame.
    {
        const double myAlt = dd(g_ref.elev);
        const xr::radio::Node me = aircraftNode(dd(g_ref.lat), dd(g_ref.lon), myAlt, g_myAglM);
        const double myAnt = myAlt + xr::radio::kAircraftAntennaM;
        const std::vector<uint32_t> talking = xr::voice::activeSpeakers();
        auto isTalking = [&](uint32_t sid) {
            return std::find(talking.begin(), talking.end(), sid) != talking.end();
        };
        const uint32_t myKhz = comKhz(txCom());
        auto freqOf = [&](uint32_t sid, uint32_t fallback) {
            const uint32_t f = xr::voice::speakerFreq(sid);
            return f ? f : (fallback ? fallback : xr::kGuardKhz);
        };
        for (const auto& kv : g_remote) {
            const Remote& rm = kv.second;
            applySignal(rm.sid, remoteNode(rm), me, rm.altMslM + xr::radio::kAircraftAntennaM, myAnt,
                        freqOf(rm.sid, myKhz), isTalking(rm.sid), g_elapsed);
        }
        const uint32_t c1 = comKhz(1), c2 = comKhz(2);
        for (const AtcStation& st : g_atcList) {
            if (!st.sid || !st.hasPos) continue;   // an ATIS row, or no position yet: as strong as ever
            const bool tuned = st.freqKhz && (st.freqKhz == c1 || st.freqKhz == c2);
            applySignal(st.sid, stationNode(st), me, (double)st.altMslM + st.antennaM, myAnt,
                        freqOf(st.sid, st.freqKhz), tuned || isTalking(st.sid), g_elapsed);
        }
        g_shadow.tick(g_elapsed, kShadowProbesPerFrame, kShadowMaxMs);
        g_shadow.forgetIdle(g_elapsed, 30.0);
    }

    // Which COM the PTT keys, published for the network thread's voice packets.
    g_txFreqKhz.store(comKhz(txCom()));

    // The audio panel's volume knobs, so turning a radio down in the cockpit
    // turns it down in the headset. A missing dataref reads 0, which would
    // mute everything, so an absent knob counts as fully up.
    // A radio with no power is silent in both directions: zero volume here
    // stops anything already in the mixer, and the position packet has
    // already told the server we are not listening.
    const float vol1 = comPowered(1) ? (g_ref.volCom1 ? fd(g_ref.volCom1) : 1.f) : 0.f;
    const float vol2 = comPowered(2) ? (g_ref.volCom2 ? fd(g_ref.volCom2) : 1.f) : 0.f;
    xr::voice::setRadioVolumes(comKhz(1), vol1, comKhz(2), vol2);
    // Guard reaches every aeroplane with a working radio, on whichever is
    // turned up louder. With the avionics off it reaches this one too, and is
    // heard exactly as much as anything else is: not at all.
    xr::voice::setGuardGain(anyComPowered() ? std::max(vol1, vol2) : 0.f);

    // Switching the avionics off mid-transmission has to unkey us.
    if (g_pttDown && !anyComPowered()) setPtt(false);

    // So does the stuck-mic timer; see pttTimeoutS().
    if (g_pttDown && g_elapsed - g_pttDownAt > pttTimeoutS()) {
        setPtt(false);
        char line[120];
        snprintf(line, sizeof(line),
                 "transmitter timed out after %.0f s -- release the key and press again",
                 (double)pttTimeoutS());
        addChat(line);
        pushNotice(line, 1);
        logMsg("%s", line);
    }

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
        detectAircraft();
        if (g_connected && (effectiveIcao() != g_loginIcao || effectiveLivery() != g_loginLivery)) {
            if (g_acfSentAt < 0.f || g_acfAcked) {
                logMsg("aircraft changed to %s, telling the server", effectiveIcao().c_str());
                pushNotice("Aircraft: " + g_acf.icao + " (" + g_acf.source + ")", 2);
                sendAircraft();
            } else if (g_elapsed - g_acfSentAt > 3.0f) {
                // no receipt: a server from before PT_AIRCRAFT -- log in again with the new type
                logMsg("aircraft changed to %s, the server did not take PT_AIRCRAFT: re-logging in", effectiveIcao().c_str());
                g_acfSentAt = -1.f;
                reconnect();
                return -1.0f;
            }
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
        g_ducting = false;
        g_shadow.clear();
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
    if (down) g_pttDownAt = g_elapsed;
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
    if (g_cfg.towerKey > 0 && (unsigned char)vk == (unsigned char)g_cfg.towerKey && (flags & xplm_DownFlag) && !(flags & xplm_UpFlag)) { xr::tower::toggle(); return 0; }
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

    // The toolbar. Settings first and in green, because it is the one a new
    // pilot is looking for. Hosting adds a button that puts the address a
    // friend has to type on the clipboard -- nothing drawn in an X-Plane
    // window can be selected with the mouse, so without it the address is
    // something you read out loud.
    g_mainButtons.clear();
    {
        struct Btn { const char* label; MainAction action; };
        std::vector<Btn> btns = {{"[ Settings ]", kBtnSettings},
                                 {"[ Reconnect ]", kBtnReconnect},
                                 {"[ Hide ]", kBtnHide}};
        if (xr::relay::running() && shareAddressPlain().find('<') == std::string::npos)
            btns.push_back({"[ Copy address ]", kBtnCopy});
        int bx = x;
        for (const Btn& bt : btns) {
            const int w = (int)XPLMMeasureString(xplmFont_Proportional, bt.label,
                                                 (int)strlen(bt.label));
            if (bx + w > r - 8) break;                 // a narrow window drops the rest
            XPLMDrawString(bt.action == kBtnSettings ? green : grey, bx, y,
                           (char*)bt.label, nullptr, xplmFont_Proportional);
            g_mainButtons.push_back({bx, bx + w, y, bt.action});
            bx += w + 14;
        }
        // What the last press of Copy did, for a few seconds: a clipboard
        // operation that silently did nothing is worse than no button.
        if (g_elapsed - g_mainCopyAt < 4.f) {
            const char* note = g_mainCopyOk ? "copied" : "could not copy";
            if (bx + 110 < r)
                XPLMDrawString(g_mainCopyOk ? green : amber, bx, y, (char*)note, nullptr,
                               xplmFont_Proportional);
        }
    }
    y -= 18;

    drawFit(g_connected ? green : amber, x, y, r, g_status, xplmFont_Proportional);
    y -= 16;
    char who[200];
    // The network thread writes the endpoint while it opens the socket, so it
    // is only read here once that thread has said it is done. Reading it
    // earlier was a data race ThreadSanitizer found: a string read while
    // another thread was halfway through assigning it.
    const std::string ep = g_sockReady.load() ? g_sock.endpoint() : std::string();
    snprintf(who, sizeof(who), "%s as %s (%s%s%s)",
             ep.empty() ? "no server" : ep.c_str(),
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
        if (!xr::weather::available())
            sky = "my own (X-Plane 11 cannot share weather)";
        else if (someoneElseIsTheSky())
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
             comKhz(1) / 1000.0, p1 ? "" : " (no power)",
             comKhz(2) / 1000.0, p2 ? "" : " (no power)",
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
    y -= 16;

    // v6: the controllers online, one per row, each a click away from being
    // tuned; the ATIS of a frequency we have tuned; and where our flight
    // plan stands. Only drawn when there is something to say.
    g_atcRows.clear();
    if (g_connected && !g_atcList.empty()) {
        drawFit(white, x, y, r, "ATC online  (click a station: COM1, an ATIS: COM2)",
                xplmFont_Proportional);
        y -= 15;
        const uint32_t c1 = comKhz(1), c2 = comKhz(2);
        for (const auto& s : g_atcList) {
            if (y < b + 120) break;
            const bool tuned = s.freqKhz && (s.freqKhz == c1 || s.freqKhz == c2);
            char row[160];
            snprintf(row, sizeof(row), "  %-10s %-8s %s%s", s.callsign.c_str(), atcKindName(s.kind),
                     freqLabel(s.freqKhz).c_str(), tuned ? "   <- tuned" : "");
            drawFit(tuned ? green : white, x, y, r, row, xplmFont_Basic);
            g_atcRows.push_back({y, s.freqKhz, s.kind == xr::ATC_ATIS ? 2 : 1, s.callsign});
            y -= 14;
        }
        y -= 4;
    }
    for (const auto& kv : g_atis) {
        const AtisText& a = kv.second;
        if (a.freqKhz != comKhz(1) && a.freqKhz != comKhz(2)) continue;
        if (g_elapsed - a.at > 600.f) continue;        // a station that went quiet
        if (y < b + 120) break;
        char head[96];
        snprintf(head, sizeof(head), "ATIS %s information %c:", a.icao.c_str(), a.letter);
        drawFit(amber, x, y, r, head, xplmFont_Proportional);
        y -= 14;
        // The text is long; two lines of it is what fits, the rest is in the
        // radio log where it arrived.
        const size_t perLine = (size_t)std::max(20, (r - x - 8) / 7);
        for (size_t at = 0; at < a.text.size() && y >= b + 120; at += perLine) {
            drawFit(white, x + 10, y, r, a.text.substr(at, perLine), xplmFont_Basic);
            y -= 14;
            if (at + perLine < a.text.size() && at + 2 * perLine >= a.text.size() + perLine) break;
        }
        y -= 4;
    }
    if (g_connected && (g_fp.filed || g_fp.status)) {
        char line[200];
        const char* st = g_fp.status == xr::FP_ACK ? "acknowledged by ATC" :
                         g_fp.status == xr::FP_CLEARED ? "cleared" :
                         g_fp.status == xr::FP_CANCELLED ? "cancelled" : "filed, waiting for ATC";
        if (g_fp.squawk)
            snprintf(line, sizeof(line), "Flight plan %s -> %s: %s, squawk %04u%s", g_fp.dep.c_str(),
                     g_fp.dest.c_str(), st, (unsigned)g_fp.squawk,
                     ownSquawk() == g_fp.squawk ? "" : "  (not set yet)");
        else
            snprintf(line, sizeof(line), "Flight plan %s -> %s: %s", g_fp.dep.c_str(), g_fp.dest.c_str(), st);
        drawFit(g_fp.status == xr::FP_ACK || g_fp.status == xr::FP_CLEARED ? green : grey, x, y, r, line,
                xplmFont_Basic);
        y -= 16;
    }
    y -= 6;

    // What TCAS can actually see, which is not the same as who is out there.
    // An aircraft answers an interrogation only with its transponder above
    // standby, and the interrogation comes from us, so ours has to be up too.
    // Everyone else is still drawn out of the window and still on the radio:
    // neither needs a transponder, and that is the point of modelling it.
    const uint8_t myXpdr   = ownXpdrMode();
    const bool    myXpdrUp = xr::xpdrTransmitting(myXpdr);
    std::vector<const Remote*> contacts;
    if (myXpdrUp) {
        for (const auto& kv : g_remote) {
            if (kv.second.ai && !g_cfg.showAiTraffic) continue;      // listed only if wanted
            if (xr::xpdrTransmitting(kv.second.xpdrMode)) contacts.push_back(&kv.second);
        }
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

    // Column headings, laid out with the same widths as the rows below, so a
    // list of a dozen contacts reads as a table rather than as numbers.
    if (!contacts.empty() && y >= b + 100) {
        char cols[96];
        snprintf(cols, sizeof(cols), "%-8s %-5s %8s  %8s  %6s  %s",
                 "callsign", "type", "altitude", "distance", "speed", "squawk");
        drawFit(grey, x, y, r, cols, xplmFont_Basic);
        y -= 14;
    }

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
        snprintf(line, sizeof(line), "%-8s %-5s %s  %5.1f nm  %3.0f kt  %04u%s%s%s",
                 rm.callsign.c_str(), rm.acIcao.c_str(), alt,
                 distanceNm(myLat, myLon, rm.lat, rm.lon), rm.gsMs * 1.94384f,
                 (unsigned)rm.squawk, rm.xpdrIdent ? " ID" : "",
                 rm.ai ? (rm.drawn ? "  AI" : "  AI (not drawn)") : "",
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
    //
    // Which means working out how many rows fit and starting that far from
    // the end. Walking the log from the beginning and stopping when the room
    // runs out does the opposite -- it shows the oldest chatter and drops the
    // call that just came in, which is the one the pilot is waiting for.
    const int inputY = b + 12;

    // The @ list, when a callsign is being chosen. It sits directly above the
    // Say line and borrows its rows from the log for as long as it is up.
    struct AtRow { std::string text; std::string callsign; int colour; };
    std::vector<AtRow> atRows;
    g_atRowsY.clear();
    g_atShown.clear();
    if (atActive()) {
        const std::string typed = g_chatInput.substr(1);
        const auto cands = atCandidates(typed);
        const int shown = std::min<int>((int)cands.size(), kAtRows);
        if (g_atPick >= shown) g_atPick = shown > 0 ? shown - 1 : 0;
        for (int i = 0; i < shown; ++i) {
            const AtCandidate& c = cands[(size_t)i];
            char row[96], dist[24] = "";
            if (c.distNm >= 0) snprintf(dist, sizeof(dist), "%.1f nm", c.distNm);
            snprintf(row, sizeof(row), "%s @%-8s %-5s %s", i == g_atPick ? ">" : " ",
                     c.callsign.c_str(), c.acIcao.c_str(), dist);
            atRows.push_back({row, c.callsign, i == g_atPick ? 2 : 0});
        }
        if ((int)cands.size() > shown) {
            char more[64];
            snprintf(more, sizeof(more), "  ...and %d more -- keep typing",
                     (int)cands.size() - shown);
            atRows.push_back({more, "", 1});
        }
        if (cands.empty()) {
            std::string why;
            if (!g_connected)       why = "  (not connected)";
            else if (typed.empty()) why = "  (nobody else is in this flight)";
            else                    why = "  (nobody in this flight is called " + typed + "...)";
            atRows.push_back({why, "", 1});
        } else {
            atRows.push_back({"  Tab or Enter picks, arrows move, or click a name", "", 1});
        }
    }
    const int atFloor = inputY + 16;        // the lowest @ row
    const int logFloor = inputY + 22 + (int)atRows.size() * 14;

    const int room = y - logFloor;
    const int fits = room < 0 ? 0 : room / 14 + 1;
    size_t first = 0;
    if (g_chatLog.size() > (size_t)fits) first = g_chatLog.size() - (size_t)fits;
    for (size_t i = first; i < g_chatLog.size(); ++i) {
        if (y < logFloor) break;
        drawFit(white, x, y, r, g_chatLog[i], xplmFont_Basic);
        y -= 14;
    }

    // Top to bottom, first choice at the top, the hint nearest the Say line.
    for (size_t i = 0; i < atRows.size(); ++i) {
        const int ry = atFloor + (int)(atRows.size() - 1 - i) * 14;
        const AtRow& a = atRows[i];
        float* col = a.colour == 2 ? green : (a.colour == 1 ? grey : white);
        drawFit(col, x, ry, r, a.text, xplmFont_Basic);
        if (!a.callsign.empty()) {
            g_atRowsY.push_back(ry);
            g_atShown.push_back(a.callsign);
        }
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
                 "Say:   (click to type, Enter to send  ·  @ to message one pilot"
                 "  ·  121.500 is heard by all)");
    }
    drawFit(g_chatFocus ? green : (g_chatInput.empty() ? amber : white), x, inputY, r,
            prompt, xplmFont_Basic);
}

// What the toolbar buttons do.
void mainButton(MainAction action) {
    switch (action) {
        case kBtnSettings:  openSettings(); break;
        case kBtnReconnect: loadConfig(); reconnect(); break;   // same as the menu item
        case kBtnHide:      XPLMSetWindowIsVisible(g_window, 0); break;
        case kBtnCopy: {
            std::string err;
            g_mainCopyOk = xr::clipboard::set(shareAddressPlain(), &err);
            g_mainCopyAt = g_elapsed;
            if (!g_mainCopyOk) logMsg("could not copy the address: %s", err.c_str());
            break;
        }
    }
}

// A toolbar button does its thing. Clicking the "Say:" row takes the
// keyboard; clicking anywhere else in the window gives it back, so the sim's
// own key bindings keep working.
int mainWindowClick(XPLMWindowID win, int x, int y, XPLMMouseStatus status, void*) {
    if (status != xplm_MouseDown) return 1;
    for (const MainButton& bt : g_mainButtons) {
        if (y >= bt.y - 4 && y <= bt.y + 12 && x >= bt.x0 && x <= bt.x1) {
            mainButton(bt.action);
            return 1;
        }
    }
    // A name in the @ list: fill it in and keep typing.
    for (size_t i = 0; i < g_atRowsY.size() && i < g_atShown.size(); ++i) {
        if (y >= g_atRowsY[i] - 4 && y <= g_atRowsY[i] + 11) {
            atComplete(g_atShown[i]);
            g_chatFocus = true;
            XPLMTakeKeyboardFocus(win);
            return 1;
        }
    }
    // v6: a station in the ATC list tunes a radio to it.
    for (const AtcRow& row : g_atcRows) {
        if (y >= row.y - 4 && y <= row.y + 11) {
            tuneRadio(row.radio, row.freqKhz);
            return 1;
        }
    }
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

    // While a callsign is being chosen, Tab and Enter take the highlighted
    // name and the arrows move the highlight. Enter cannot mean "send" here
    // anyway: "@ESN" with nothing after it is not a message.
    if (atActive()) {
        const int shown = std::min<int>((int)atCandidates(g_chatInput.substr(1)).size(), kAtRows);
        const bool pick = uvk == XPLM_VK_TAB || ch == '\t' || uvk == XPLM_VK_RETURN ||
                          uvk == XPLM_VK_ENTER || ch == '\r' || ch == '\n';
        if (shown > 0 && pick) {
            const auto cands = atCandidates(g_chatInput.substr(1));
            atComplete(cands[(size_t)std::min(std::max(g_atPick, 0), shown - 1)].callsign);
            return;
        }
        if (shown > 0 && uvk == XPLM_VK_DOWN) { g_atPick = (g_atPick + 1) % shown; return; }
        if (shown > 0 && uvk == XPLM_VK_UP)   { g_atPick = (g_atPick + shown - 1) % shown; return; }
        if (uvk == XPLM_VK_TAB || ch == '\t') return;     // nothing to complete
    }

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
        g_atPick = 0;
        return;
    }
    if (ch >= 32 && ch < 127 && g_chatInput.size() < 200) {
        g_chatInput += (char)ch;
        g_atPick = 0;                      // the list changed; start at its top
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

// A settings change restarts the server on a worker thread: relay::stop() joins the server's thread (up to one
// 20 ms receive wait) and start() binds a port, and the sim should not sit through either. pollHosting()
// collects the answer on the flight loop and does what belongs on the main thread: the router request, the
// log line, and the reconnect that was waiting for the server.
struct HostingJob { bool enable = false; uint16_t port = 0; std::string password; bool ducting = false, upnp = false; };
struct HostingResult { bool done = false, enable = false, wasRunning = false, ok = false, upnp = false; uint16_t port = 0; std::string err; };
std::thread   g_hostWorker;
std::mutex    g_hostMx;
HostingResult g_hostResult;                // guarded by g_hostMx: the worker's answer until pollHosting takes it
bool          g_reconnectAfterHosting = false;   // main thread only

HostingJob hostingJob() {
    HostingJob j;
    j.enable = g_cfg.hostEnabled; j.port = (uint16_t)g_cfg.hostPort_i(); j.password = g_cfg.password;
    j.ducting = g_cfg.hostDucting; j.upnp = g_cfg.hostUpnp;
    return j;
}

// The server half: either thread.
HostingResult runHosting(const HostingJob& j) {
    HostingResult r;
    r.enable = j.enable; r.port = j.port; r.upnp = j.upnp;
    r.wasRunning = xr::relay::running();
    if (!j.enable) { xr::relay::stop(); r.ok = true; return r; }
    r.ok = xr::relay::start(j.port, j.password, &r.err, j.ducting);   // stops the old one first
    return r;
}

// The rest: main thread.
void finishHosting(const HostingResult& r) {
    if (!r.enable) {
        if (r.wasRunning) {
            xr::upnp::releaseAsync();
            logMsg("hosting stopped");
        }
        g_hostNote.clear();
        return;
    }
    if (!r.ok) {
        g_hostNote = "cannot host: " + r.err;
        logMsg("%s", g_hostNote.c_str());
        return;
    }
    g_hostNote.clear();
    logMsg("hosting on port %u", (unsigned)r.port);
    xr::upnp::clear();
    xr::upnp::requestAsync(r.port, "XRadio", r.upnp);
}

// At startup, before anything needs the server: on the spot.
void applyHosting() { finishHosting(runHosting(hostingJob())); }

void pollHosting() {
    HostingResult r;
    {
        std::lock_guard<std::mutex> lk(g_hostMx);
        if (!g_hostResult.done) return;
        r = g_hostResult;
        g_hostResult = HostingResult{};
    }
    if (g_hostWorker.joinable()) g_hostWorker.join();   // it has answered, so it has finished
    finishHosting(r);
    if (g_reconnectAfterHosting) { g_reconnectAfterHosting = false; reconnect(); }
}

// A settings change: the server restarts on the worker; a reconnect the change needs waits for it, so the
// client has something to connect to by the time it tries.
void applyHostingAsync(bool reconnectAfter) {
    if (g_hostWorker.joinable()) g_hostWorker.join();   // the previous change: a few ms of work
    pollHosting();                                        // ...and its answer, if not collected yet
    g_reconnectAfterHosting = g_reconnectAfterHosting || reconnectAfter;
    const HostingJob job = hostingJob();
    g_hostWorker = std::thread([job] {
        HostingResult r;
        try {
            r = runHosting(job);
        } catch (const std::exception& e) {
            r.enable = job.enable; r.port = job.port; r.ok = false; r.err = e.what();
        } catch (...) {
            r.enable = job.enable; r.port = job.port; r.ok = false; r.err = "unknown exception";
        }
        r.done = true;
        std::lock_guard<std::mutex> lk(g_hostMx);
        g_hostResult = r;
    });
}

// The router half on its own, for when "Ask the router to open it" changes
// while the server keeps running. Switched off, the mapping is given back
// and only the public address is looked up, for the forward-it-yourself
// instructions on the Hosting tab.
void applyRouter() {
    if (!xr::relay::running()) return;
    const uint16_t port = (uint16_t)g_cfg.hostPort_i();
    xr::upnp::clear();
    if (!g_cfg.hostUpnp) xr::upnp::releaseAsync();
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
    logMsg("reconnecting to %s:%d", g_cfg.activeHost().c_str(), g_cfg.activePort());
    netStop();
    sendLogout();
    g_connected = false;
    g_sessionId.store(0);
    g_remote.clear();
    xr::csl::removeAll();
    g_atcList.clear(); g_atcListAt = -1000.f;   // another server, another set of controllers
    g_ducting = false;                            // ...and another sky
    g_shadow.clear();
    g_atis.clear();
    g_fp = FlightPlanState{};
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

// S3: the OFP becomes the typed flight plan -- editable before filing. The callsign stays the pilot's own.
void applyOfp(const xr::simbrief::Ofp& o) {
    g_cfg.fpRules = "I";
    g_cfg.fpDep = o.dep; g_cfg.fpDest = o.dest; g_cfg.fpAltn = o.altn;
    g_cfg.fpCruise = std::to_string(o.cruiseFt);
    std::string route = o.routeAtc.empty() ? o.route : o.routeAtc;
    if (!o.sid.empty() && route.find(o.sid) == std::string::npos) route = o.sid + " " + route;
    if (!o.star.empty() && route.find(o.star) == std::string::npos) route += " " + o.star;
    g_cfg.fpRoute = route.substr(0, 127);
    // What the 64-byte remarks can carry for the controller: TowerTrainer reads EOBT/TAS/REG/EQPT and the SIMBRIEF mark.
    std::string rem = "SIMBRIEF";
    if (!o.eobt.empty()) rem += " EOBT/" + o.eobt;
    if (o.tas) rem += " TAS/" + std::to_string(o.tas);
    if (!o.reg.empty()) rem += " REG/" + o.reg;
    if (!o.equip.empty()) rem += " EQPT/" + o.equip;
    g_cfg.fpRemarks = rem.substr(0, 63);
    g_edit.fpRules = g_cfg.fpRules; g_edit.fpDep = g_cfg.fpDep; g_edit.fpDest = g_cfg.fpDest; g_edit.fpAltn = g_cfg.fpAltn;
    g_edit.fpCruise = g_cfg.fpCruise; g_edit.fpRoute = g_cfg.fpRoute; g_edit.fpRemarks = g_cfg.fpRemarks;
    xr::saveSettings(g_cfg, g_cfgPath);
}
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
    xr::relay::setDucting(g_cfg.hostDucting);     // realism for the flight we host: no restart
    xr::voice::setVolume(g_cfg.volume);
    xr::voice::setSidetone(g_cfg.sidetone);
    xr::voice::setHiss(g_cfg.hiss);
    xr::voice::setRadioFilter(g_cfg.radioFilter);
    xr::voice::setSquelch(g_cfg.squelch);
    xr::voice::setCabinNoise(g_cfg.cabinNoise);
    xr::Smoother::setDefaultPlayout(g_cfg.smoothMs / 1000.0);
    g_pttKeyVk = g_cfg.pttKey;
    xr::csl::setTrafficVisible(g_cfg.showTraffic);
    xr::csl::setLabels(g_cfg.showLabels, g_cfg.labelDistNm);
    g_sendInterval = 1.0f / (g_cfg.reportHz < 1.f ? 1.f : g_cfg.reportHz);
    xr::tower::applyConfig(towerConfig());
}

void openSettings() {
#ifdef XRADIO_UI_IMGUI
    xr::ui2::host::showSettings();
    return;
#endif
    if (!g_settingsWin) return;
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

// The text window's; the ImGui build never shows it and does not come here (applySettings).
void closeSettings() {
    g_ui.focus = -1;
    g_captureKey = false;
    g_keyQueue.clear();
    if (!g_settingsWin) return;
    if (XPLMHasKeyboardFocus(g_settingsWin)) XPLMTakeKeyboardFocus(nullptr);
    XPLMSetWindowIsVisible(g_settingsWin, 0);
}

// Save what the settings window holds and push it into everything that cares. Runs on the main thread and
// never inside an ImGui frame: the ImGui window's Apply gets here through requestApply() and the flight loop.
// The text window calls it from its own draw, which has no ImGui in it, and even there only the quick part
// happens on the spot -- the hosted server restarts on a worker, the audio devices and the reconnect wait for
// the flight loop. False, with the reason in g_settingsNote, when the settings do not validate.
bool applySettings() {
    g_edit.callsign = upper(trim(g_edit.callsign));
    g_edit.acIcao   = upper(trim(g_edit.acIcao));
    g_edit.host     = trim(g_edit.host);
    g_edit.password = trim(g_edit.password);

    const std::string problem = xr::validate(g_edit);
    if (!problem.empty()) {
        g_settingsNote = problem;
        return false;
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
    const bool relayChanged = (g_edit.hostEnabled != g_cfg.hostEnabled) ||
                              (g_edit.hostPort != g_cfg.hostPort) ||
                              (g_edit.password != g_cfg.password);
    // The router question on its own does not touch the server. It used to
    // restart it, which threw every connected pilot off for the 12 s it
    // takes them to notice and log in again.
    const bool routerChanged = !relayChanged && g_edit.hostEnabled &&
                               (g_edit.hostUpnp != g_cfg.hostUpnp);
    const bool devChanged = (g_edit.micDevice != g_cfg.micDevice) ||
                            (g_edit.outDevice != g_cfg.outDevice);

    g_settingsNote.clear();
    g_cfg = g_edit;
    if (!xr::saveSettings(g_cfg, g_cfgPath)) {
        // applied all the same -- the settings are live -- but the next start would not have them
        logMsg("could not write %s: the settings are applied but not saved", g_cfgPath.c_str());
        pushNotice("Settings applied, but xradio.cfg could not be written", 1);
    }
    applyLiveSettings();
    // Start or stop the built-in server before reconnecting, so the client
    // has something to connect to by the time it tries: a change to the
    // server reconnects once the worker has it running.
    if (relayChanged) applyHostingAsync(netChanged);
    else if (routerChanged) applyRouter();

    if (devChanged) {
        defer("audio devices", [] {
            std::string err;
            if (!xr::voice::reopenDevices(g_cfg.micDevice, g_cfg.outDevice, &err)) {
                logMsg("could not switch audio device: %s", err.c_str());
            } else {
                logMsg("audio devices: mic '%s', out '%s'",
                       xr::voice::currentMic().c_str(), xr::voice::currentOutput().c_str());
            }
        });
    }
#ifndef XRADIO_UI_IMGUI
    closeSettings();
#endif
    if (netChanged && !relayChanged) defer("reconnect", [] { reconnect(); });
    return true;
}

// The ImGui window's Apply, from the flight loop (requestApply).
void runApply() {
    g_edit = g_applyWanted;
    if (!applySettings()) {
        logMsg("settings not saved: %s", g_settingsNote.c_str());
#ifdef XRADIO_UI_IMGUI
        xr::ui2::state().editDirty = true;           // Apply stays pressable for the correction
        xr::ui2::host::toast("Not saved: " + g_settingsNote, 3);
#endif
        return;
    }
#ifdef XRADIO_UI_IMGUI
    xr::ui2::host::applyDisplay(g_cfg);
#endif
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
                               std::string(f.unit) == "%", f.decimals);
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

    // v6: the ATC tab files the plan typed above, and says where it stands.
    if (g_tab == 4) {
        g_ui.nextRow();
        static const char* fpBtns[] = {"File flight plan", "Cancel flight plan"};
        const int hit = xr::ui::buttons(g_ui, fpBtns, 2);
        if (hit == 0) {
            // What is typed is what gets filed, saved or not.
            g_cfg.fpRules = g_edit.fpRules; g_cfg.fpDep = g_edit.fpDep; g_cfg.fpDest = g_edit.fpDest;
            g_cfg.fpAltn = g_edit.fpAltn; g_cfg.fpCruise = g_edit.fpCruise;
            g_cfg.fpRoute = g_edit.fpRoute; g_cfg.fpRemarks = g_edit.fpRemarks;
            const std::string problem = sendFlightPlan();
            g_settingsNote = problem.empty() ? "Flight plan filed -- ATC answers in the XRadio window" : "Not filed: " + problem;
        } else if (hit == 1) {
            cancelFlightPlan();
            g_settingsNote = "Flight plan cancelled";
        }
        g_ui.nextRow();
        static const char* sbBtns[] = {"Import from SimBrief"};
        if (xr::ui::buttons(g_ui, sbBtns, 1) == 0) {
            importSimbrief();
            g_settingsNote = g_cfg.simbriefUser.empty() ? "Enter your SimBrief username or pilot ID above first" : "Fetching your latest OFP from SimBrief...";
        }
        if (g_ofp.ok) xr::ui::text(g_ui, ("OFP: " + g_ofp.summary()).c_str(), 2);
        if (!g_atcList.empty()) {
            std::string who = "Online:";
            for (const auto& s : g_atcList) who += " " + s.callsign + " " + freqLabel(s.freqKhz);
            xr::ui::text(g_ui, who.c_str(), 2);
        } else {
            xr::ui::text(g_ui, g_connected ? "No ATC station online in this flight" : "Not connected", 1);
        }
        if (g_fp.filed || g_fp.status) {
            char line[160];
            snprintf(line, sizeof(line), "Plan %s -> %s: %s%s%04u", g_fp.dep.c_str(), g_fp.dest.c_str(),
                     g_fp.status == xr::FP_ACK ? "acknowledged" : g_fp.status == xr::FP_CLEARED ? "cleared" :
                     g_fp.status == xr::FP_CANCELLED ? "cancelled" : "filed",
                     g_fp.squawk ? ", squawk " : "", (unsigned)g_fp.squawk);
            if (!g_fp.squawk) line[strlen(line) - 4] = '\0';   // no ", squawk 0000"
            xr::ui::text(g_ui, line, 0);
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

// The notice window. No title bar, no background of its own beyond what
// X-Plane draws, and it takes no clicks: a pilot flying with the XRadio window
// shut wants to read it, not to have something else on screen to catch the
// mouse.
XPLMWindowID g_noticeWin = nullptr;

void drawNotices(XPLMWindowID win, void*) {
    int l, t, r, b;
    XPLMGetWindowGeometry(win, &l, &t, &r, &b);

    float white[] = {1.f, 1.f, 1.f};
    float amber[] = {1.f, 0.8f, 0.3f};
    float green[] = {0.4f, 1.f, 0.4f};

    int y = t - 14;
    for (const auto& n : g_notices) {
        float* col = n.colour == 1 ? amber : (n.colour == 2 ? green : white);
        XPLMDrawString(col, l + 6, y, (char*)n.text.c_str(), nullptr,
                       xplmFont_Proportional);
        y -= 16;
    }
}

// Expire what has had its seconds, and put the window where the notices are
// -- which is nowhere at all when there are none, or when the main window is
// open and already showing the same thing in its radio log.
void updateNotices() {
    for (size_t i = 0; i < g_notices.size();) {
        if (g_elapsed - g_notices[i].at > noticeHoldS()) g_notices.erase(g_notices.begin() + (long)i);
        else ++i;
    }
    if (!g_noticeWin) return;

    const bool want = !g_notices.empty() && !XPLMGetWindowIsVisible(g_window);
    if (!want) {
        if (XPLMGetWindowIsVisible(g_noticeWin)) XPLMSetWindowIsVisible(g_noticeWin, 0);
        return;
    }

    int widest = 0;
    for (const auto& n : g_notices) {
        const int w = (int)XPLMMeasureString(xplmFont_Proportional, (char*)n.text.c_str(),
                                             (int)n.text.size());
        if (w > widest) widest = w;
    }
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const int w = std::min(std::max(widest + 16, 200), std::max(240, sr - sl - 60));
    const int h = (int)g_notices.size() * 16 + 12;
    const int left = sl + 20;
    const int top  = st - 45;            // clear of the menu bar
    XPLMSetWindowGeometry(g_noticeWin, left, top, left + w, top - h);
    if (!XPLMGetWindowIsVisible(g_noticeWin)) XPLMSetWindowIsVisible(g_noticeWin, 1);
}

void createNoticeWindow() {
    XPLMCreateWindow_t p{};
    p.structSize           = sizeof(p);
    p.left = 0; p.top = 0; p.right = 0; p.bottom = 0;   // placed by updateNotices
    p.visible              = 0;
    p.drawWindowFunc       = drawNotices;
    // Everything below returns 0: not handled, so the click goes through to
    // whatever is underneath. A notice must never eat a click on the panel.
    p.handleMouseClickFunc = [](XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 0; };
    p.handleRightClickFunc = [](XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 0; };
    p.handleMouseWheelFunc = [](XPLMWindowID, int, int, int, int, void*) { return 0; };
    p.handleKeyFunc        = [](XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {};
    p.handleCursorFunc     = [](XPLMWindowID, int, int, void*) -> XPLMCursorStatus {
        return xplm_CursorDefault;
    };
    p.layer                = xplm_WindowLayerFloatingWindows;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    g_noticeWin = XPLMCreateWindowEx(&p);
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
#ifdef XRADIO_UI_IMGUI
    // the text windows stay hidden in this build: only the ImGui ones are put back
    xr::ui2::host::resetPositions();
    xr::ui2::host::resetTowerPositions();
#else
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
#endif
    logMsg("window positions reset");
}

// Tower mode (v0.6.0): the commands a pilot binds, and the settings the module gets
xr::tower::Config towerConfig() {
    xr::tower::Config c;
    c.host = g_cfg.towerHost; c.port = g_cfg.towerPort; c.code = g_cfg.towerCode; c.position = g_cfg.towerPos; c.name = g_cfg.towerName;
    c.park = g_cfg.towerPark; c.syncWeather = g_cfg.towerWeather; c.cabHeightM = g_cfg.towerHeight; c.draw3d = g_cfg.tower3d; c.showWindows = g_cfg.towerWindows;
    return c;
}
int towerCmdHandler(XPLMCommandRef, XPLMCommandPhase phase, void* ref) {
    const int which = (int)(intptr_t)ref;
    const bool begin = phase == xplm_CommandBegin, held = phase != xplm_CommandEnd;
    switch (which) {
        case 0: if (begin) xr::tower::toggle(); break;
        case 1: if (held) xr::tower::look(-1.5f, 0.f); break;
        case 2: if (held) xr::tower::look(1.5f, 0.f); break;
        case 3: if (held) xr::tower::look(0.f, 1.f); break;
        case 4: if (held) xr::tower::look(0.f, -1.f); break;
        case 5: if (held) xr::tower::zoom(1.04f); break;
        case 6: if (held) xr::tower::zoom(0.96f); break;
        case 7: if (begin) xr::tower::groundView(); break;
        case 8: if (begin) xr::tower::towerView(); break;
        case 9: if (begin) xr::tower::nextTarget(); break;
        case 10: if (begin) xr::tower::setWindowsVisible(!xr::tower::windowsVisible()); break;
        default: break;
    }
    return 0;
}
const struct { const char* name; const char* desc; } kTowerCmds[] = {
    {"xradio/tower/toggle",      "XRadio: tower mode on / off (TowerTrainer)"},
    {"xradio/tower/look_left",   "XRadio tower: look left"},
    {"xradio/tower/look_right",  "XRadio tower: look right"},
    {"xradio/tower/look_up",     "XRadio tower: look up"},
    {"xradio/tower/look_down",   "XRadio tower: look down"},
    {"xradio/tower/zoom_in",     "XRadio tower: binoculars in"},
    {"xradio/tower/zoom_out",    "XRadio tower: binoculars out"},
    {"xradio/tower/ground_view", "XRadio tower: view from the apron"},
    {"xradio/tower/tower_view",  "XRadio tower: back to the cab"},
    {"xradio/tower/next_target", "XRadio tower: follow the next aircraft"},
    {"xradio/tower/windows",     "XRadio tower: show / hide the tower windows"},
};

#ifdef XRADIO_UI_IMGUI
// ---------------------------------------------------------------------------
// Phase U: what the new window shows (filled every frame) and what it can ask for
// ---------------------------------------------------------------------------
uint32_t standbyKhz(int which) {
    XPLMDataRef r = XPLMFindDataRef(which == 2 ? "sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833" : "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833");
    return r ? (uint32_t)XPLMGetDatai(r) : 0;
}
void setStandbyKhz(int which, uint32_t khz) {
    XPLMDataRef r = XPLMFindDataRef(which == 2 ? "sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833" : "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833");
    if (r) XPLMSetDatai(r, (int)khz);
}
uint32_t g_guardPrevKhz = 0;

xr::ui2::Actions makeActions() {
    xr::ui2::Actions a;
    a.tune = [](int which, uint32_t khz) { tuneRadio(which, khz); };
    a.setStandby = [](int which, uint32_t khz) { setStandbyKhz(which, khz); };
    a.swap = [](int which) { const uint32_t act = comKhz(which), stby = standbyKhz(which); if (stby) tuneRadio(which, stby); if (act) setStandbyKhz(which, act); };
    a.setVolume = [](int which, float v) { XPLMDataRef r = which == 2 ? g_ref.volCom2 : g_ref.volCom1; if (r) XPLMSetDataf(r, v); };
    a.setGuard = [](bool on) {
        if (on) { g_guardPrevKhz = comKhz(2); tuneRadio(2, 121500); }
        else if (g_guardPrevKhz) { tuneRadio(2, g_guardPrevKhz); g_guardPrevKhz = 0; }
    };
    a.sendText = [](const std::string& t) { sendText(t); };
    a.fileFlightPlan = []() { return sendFlightPlan(); };
    a.cancelFlightPlan = []() { cancelFlightPlan(); };
    // These run inside the window's frame, so the heavy ones only queue their work for the flight loop
    a.applySettings = [](const xr::Settings& s) { requestApply(s); };
    a.reconnect = []() { defer("reconnect", [] { loadConfig(); reconnect(); }); };
    a.refreshDevices = []() { defer("refresh devices", [] { refreshDeviceLists(); }); };
    a.copyAddress = []() { mainButton(kBtnCopy); return std::string(g_mainCopyOk ? "copied" : "could not copy"); };
    a.toggleTower = []() { defer("tower mode", [] { xr::tower::toggle(); }); };   // the camera, the aircraft, the link
    a.keyPtt = [](bool down) { setPtt(down); };
    a.openUrl = [](const std::string& url) { xr::ui2::openUrlNative(url); };
    a.importSimbrief = []() { importSimbrief(); };
    a.writeFms = []() { return writeOfpFms(); };
    a.checkUpdate = []() {};
    return a;
}

// What the tower windows do: each is one of tower.cpp's actions (the logic stays there).
xr::ui2::TowerActions makeTowerActions() {
    xr::ui2::TowerActions a;
    a.select = [](int id) { xr::tower::select(id); };
    a.transmit = [](const std::string& t) { xr::tower::transmit(t); };
    a.accept = [](int id) { xr::tower::acceptHandoff(id); };
    a.moveStrip = [](int id, const std::string& bay) { xr::tower::moveStrip(id, bay); };
    a.setLvp = [](bool on) { xr::tower::setLvp(on); };
    a.setRunway = [](const std::string& end) { xr::tower::setRunway(end); };
    a.setStopBars = [](bool on) { xr::tower::setStopBars(on); };
    a.lightGun = [](int id, const std::string& signal) { xr::tower::lightGun(id, signal); };
    a.retry = []() { xr::tower::retry(); };
    a.leave = []() { defer("leave the tower", [] { xr::tower::leave(); }); };   // camera, aircraft and link: not mid-frame
    a.settings = []() { xr::ui2::host::showSettings(); xr::ui2::state().settingsGroup = 5; };
    a.look = [](float dYaw, float dPitch) { xr::tower::look(dYaw, dPitch); };
    a.binoculars = [](float f) { xr::tower::zoom(f); };
    a.follow = [](int id) { xr::tower::follow(id); };
    a.lookAt = [](int id) { xr::tower::lookAt(id); };
    return a;
}

xr::ui2::Model buildModel() {
    xr::ui2::Model m;
    m.elapsed = g_elapsed;
    m.settings = g_cfg;
    m.conn.connected = g_connected;
    m.conn.connecting = !g_connected && g_sockReady.load();
    m.conn.status = g_status;
    m.conn.endpoint = g_sockReady.load() ? g_sock.endpoint() : std::string();
    m.conn.callsign = g_cfg.callsign; m.conn.icao = effectiveIcao(); m.conn.livery = effectiveLivery();
    m.conn.acSource = g_acf.source; m.conn.acNote = g_acf.note; m.conn.tailnum = g_acfTail;
    m.conn.tcas = g_tcas.level == xr::TCAS_RA ? (g_tcas.sense > 0 ? "RA CLIMB" : g_tcas.sense < 0 ? "RA DESCEND" : "RA") : g_tcas.level == xr::TCAS_TA ? "TA" : "";
    m.conn.protoLocal = xr::kProtoVersion; m.conn.protoServer = g_connected ? xr::kProtoVersion : 0;
    if (g_status.find("another XRadio version") != std::string::npos) m.conn.versionMismatch = g_status;
    m.conn.lastPacketS = g_lastRxTime < 0 ? -1.f : g_elapsed - g_lastRxTime;
    m.conn.packetsPerS = g_pktRate;
    int peers = 0, ais = 0;
    for (const auto& kv : g_remote) { if (kv.second.ai) ++ais; else ++peers; }
    m.conn.peers = peers; m.conn.aiCount = ais;
    if (g_connected) {
        m.conn.skyLine = !xr::weather::available() ? "my own (X-Plane 11 cannot share weather)" : someoneElseIsTheSky() ? (g_cfg.followWeather ? "the flight's" : "the flight's (not following)") : g_cfg.shareWeather ? "mine, shared with the flight" : "my own";
    }
    const std::vector<uint32_t> speakers = xr::voice::activeSpeakers();
    float bestSignal = 0.f;
    std::string who;
    for (uint32_t sid : speakers) {
        auto it = g_remote.find(sid);
        if (who.empty()) who = it != g_remote.end() ? it->second.callsign : std::to_string(sid);
        bestSignal = std::max(bestSignal, xr::voice::signalQualityOf(sid));
    }
    m.txWho = who;
    m.pttDown = g_pttDown; m.pttDownAt = g_pttDownAt;
    for (int i = 0; i < 2; ++i) {
        xr::ui2::Com& c = m.com[i];
        c.which = i + 1; c.activeKhz = comKhz(i + 1); c.standbyKhz = standbyKhz(i + 1); c.powered = comPowered(i + 1);
        for (const auto& st : g_atcList) if (st.freqKhz && st.freqKhz == c.activeKhz) { c.stationName = st.callsign; break; }
        const bool txHere = txCom() == i + 1;
        c.tx = g_pttDown && txHere;
        c.rx = !speakers.empty() && txHere;
        c.signal = c.rx ? bestSignal : 0.f;
        const XPLMDataRef vr = i == 0 ? g_ref.volCom1 : g_ref.volCom2;
        c.volume = vr ? XPLMGetDataf(vr) : 1.f;
    }
    m.guard = comKhz(2) == 121500;
    const uint32_t c1 = comKhz(1), c2 = comKhz(2);
    for (const auto& st : g_atcList) {
        xr::ui2::Station x;
        x.callsign = st.callsign; x.kindName = atcKindName(st.kind); x.icao = st.icao; x.freqKhz = st.freqKhz;
        x.atis = st.kind == xr::ATC_ATIS; x.tuned = st.freqKhz && (st.freqKhz == c1 || st.freqKhz == c2);
        x.ai = st.callsign.find("TTRAIN") != std::string::npos;
        if (st.hasPos) x.distNm = distanceNm(dd(g_ref.lat), dd(g_ref.lon), st.lat, st.lon);
        m.atc.push_back(x);
    }
    for (const auto& kv : g_atis) {
        const AtisText& at = kv.second;
        if (at.freqKhz != c1 && at.freqKhz != c2) continue;
        if (g_elapsed - at.at > 600.f) continue;
        m.atis.emplace_back(at.icao + " information " + std::string(1, at.letter), at.text);
    }
    const double myLat = dd(g_ref.lat), myLon = dd(g_ref.lon);
    for (const auto& kv : g_remote) {
        const Remote& r = kv.second;
        xr::ui2::Pilot q;
        q.sid = r.sid; q.callsign = r.callsign; q.type = r.acIcao; q.distNm = distanceNm(myLat, myLon, r.lat, r.lon);
        q.altFt = r.altMslM * 3.28084f; q.gsKt = r.gsMs * 1.94384f; q.squawk = r.squawk; q.ai = r.ai; q.drawn = r.drawn;
        q.talking = std::find(speakers.begin(), speakers.end(), r.sid) != speakers.end();
        q.signal = q.talking ? xr::voice::signalQualityOf(r.sid) : 0.f;
        q.emergency = r.squawk == 7500 || r.squawk == 7600 || r.squawk == 7700;
        m.pilots.push_back(q);
    }
    for (const Msg& x : g_msgLog) {
        xr::ui2::Message mm;
        mm.t = x.t; mm.from = x.from; mm.text = x.text; mm.freqKhz = x.khz; mm.atc = x.atc; mm.priv = x.priv; mm.mine = x.mine; mm.guard = x.guard; mm.system = x.system;
        char tt[16]; snprintf(tt, sizeof tt, "%d:%02d", (int)x.t / 60, (int)x.t % 60); mm.timeText = tt;
        m.messages.push_back(mm);
    }
    m.fp.rules = g_cfg.fpRules; m.fp.dep = g_cfg.fpDep; m.fp.dest = g_cfg.fpDest; m.fp.altn = g_cfg.fpAltn; m.fp.type = effectiveIcao(); m.fp.callsign = g_cfg.callsign;
    m.fp.route = g_cfg.fpRoute; m.fp.cruise = g_cfg.fpCruise; m.fp.remarks = g_cfg.fpRemarks;
    m.fp.status = g_fp.status == xr::FP_ACK ? 2 : g_fp.status == xr::FP_CLEARED ? 3 : g_fp.status == xr::FP_CANCELLED ? 4 : g_fp.filed ? 1 : 0;
    m.fp.squawk = g_fp.squawk;
    m.fp.fetching = xr::simbrief::busy();
    m.fp.ofpSerial = g_ofpSerial;
    if (g_ofp.ok) {   // S3: the OFP numbers, as text
        const xr::simbrief::Ofp& o = g_ofp;
        xr::ui2::OfpView& v = m.fp.ofp;
        v.present = true; v.callsign = o.callsign; v.type = o.acType; v.reg = o.reg; v.dep = o.dep; v.dest = o.dest; v.altn = o.altn;
        v.depRwy = o.depRwy; v.destRwy = o.destRwy; v.sid = o.sid; v.star = o.star; v.route = o.routeAtc.empty() ? o.route : o.routeAtc;
        v.cruise = xr::ui2::levelText(o.cruiseFt); v.tas = std::to_string(o.tas);
        char mb[16]; snprintf(mb, sizeof mb, "M%.2f", o.mach); v.mach = o.mach > 0 ? mb : "";
        v.eobt = o.eobt; v.etd = o.etd; v.eta = o.eta; v.block = std::to_string(o.blockFuel); v.trip = std::to_string(o.tripFuel);
        v.zfw = std::to_string(o.zfw); v.tow = std::to_string(o.tow); v.ldw = std::to_string(o.ldw); v.units = o.units; v.pdfUrl = o.pdfUrl;
        m.fp.tas = v.tas; m.fp.eobt = o.eobt; m.fp.equip = o.equip;
        char sum[160]; snprintf(sum, sizeof sum, "OFP %s %s-%s %s (%d min ago)", o.callsign.c_str(), o.dep.c_str(), o.dest.c_str(), v.cruise.c_str(), (int)((g_elapsed - g_ofpAt) / 60.f));
        m.fp.simbriefAvailable = true; m.fp.simbriefSummary = sum;
    }
    if (g_fp.status == xr::FP_ACK || g_fp.status == xr::FP_CLEARED) {
        unsigned sq = 0;
        xr::ui2::parseClearance(g_fp.remarks, &m.fp.clearedSid, &m.fp.clearedLevel, &sq);
        m.fp.clearedDest = g_fp.dest; m.fp.clearance = g_fp.remarks;
    }
    {
        const xr::relay::Status st = xr::relay::status();
        const xr::upnp::Result u = xr::upnp::latest();
        m.host.running = st.running; m.host.port = st.port; m.host.clients = st.clients; m.host.callsigns = st.callsigns; m.host.error = st.error.empty() ? g_hostNote : st.error;
        m.host.mapped = u.mapped; m.host.doubleNat = u.doubleNat; m.host.upnpBusy = xr::upnp::busy(); m.host.router = u.router; m.host.upnpAsked = g_cfg.hostUpnp;
        if (st.running) {
            m.host.address = shareAddressPlain();
            if (!u.externalIp.empty()) m.host.joinCode = xr::joincode::encode(u.externalIp, st.port);
            m.host.lanAddress = lanAddress();
            if (!u.mapped && !u.error.empty()) m.host.error = u.error;
        }
    }
    m.voice.available = xr::voice::available(); m.voice.haveMic = xr::voice::haveMicrophone(); m.voice.status = xr::voice::status(); m.voice.micLevel = g_pttDown ? xr::voice::micLevel() : 0.f;
    m.voice.micDevices = g_micList; m.voice.outDevices = g_outList;
    const xr::update::Info up = xr::update::latest();
    m.update.newer = up.newer; m.update.latest = up.latest; m.update.url = up.url; m.update.current = xr::brand::version();
    m.towerActive = xr::tower::active(); m.towerStatus = xr::tower::statusLine();
    return m;
}
#endif

void menuHandler(void*, void* item) {
    intptr_t which = (intptr_t)item;
    if (which == 0) {
#ifdef XRADIO_UI_IMGUI
        xr::ui2::host::toggleMain();
#else
        if (g_window) XPLMSetWindowIsVisible(g_window, !XPLMGetWindowIsVisible(g_window));
#endif
    } else if (which == 1) {
        openSettings();
    } else if (which == 2) {
        loadConfig();
        reconnect();
    } else if (which == 3) {
        resetWindowPositions();
    } else if (which == 4) {
        sendText("Hello from " + g_cfg.callsign);
    } else if (which == 5) {
        xr::tower::toggle();
    } else if (which == 6) {
        xr::tower::setWindowsVisible(!xr::tower::windowsVisible());
    } else if (which == 7) {
        xr::tower::groundView();
    } else if (which == 8) {
        xr::tower::towerView();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// X-Plane plugin entry points
// ---------------------------------------------------------------------------
PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    strcpy(outName, "XRadio");
    strcpy(outSig,  "ee.doesvic.xradio");
    strcpy(outDesc, "Shared traffic and radio communication between X-Plane pilots.");

    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    XPLMEnableFeature("XPLM_USE_NATIVE_WIDGET_WINDOWS", 1);

    loadConfig();
    findRefs();
    {
        xr::tower::Hooks th;
        th.log = [](const std::string& s) { logMsg("%s", s.c_str()); };
        th.xradioConnected = [] { return g_connected; };
        th.xplaneRoot = [] { return xplaneRoot(); };
        th.pttDown = [] { return g_pttDown; };
        xr::tower::init(th);
    }
    xr::weather::init();
    createWindow();
    createSettingsWindow();
    // Third, deliberately: the test harness numbers windows in creation order
    // and two suites already know the settings window as number two.
    createNoticeWindow();
#ifdef XRADIO_UI_IMGUI
    // the text windows stay as the fallback build; with the ImGui window they are never shown
    if (g_window) XPLMSetWindowIsVisible(g_window, 0);
    if (g_noticeWin) XPLMSetWindowIsVisible(g_noticeWin, 0);
    // a failed IM_ASSERT is a line in Log.txt, never the end of the sim (ui2/xr_imconfig.h)
    xr::ui2::setImguiAssertSink([](const char* m) { logMsg("%s", m); });
    xr::ui2::host::init(makeActions(), g_cfg);
    // Tower mode's windows exist only here: a build without ImGui runs Tower mode (camera, link, traffic) headless
    xr::ui2::host::initTower(makeTowerActions(), g_cfg.towerLayout);
#endif

    // ICAO Doc 8643 (XPMP2 ships it in Resources/) to tell a real designator from a made-up one
    if (!g_doc8643.loadFile(pluginRootDir() + "/Resources/Doc8643.txt"))
        logMsg("Resources/Doc8643.txt not found: aircraft types are taken as the sim gives them");
    detectAircraft();
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
    for (size_t i = 0; i < sizeof(kTowerCmds) / sizeof(kTowerCmds[0]); ++i) {
        XPLMCommandRef r = XPLMCreateCommand(kTowerCmds[i].name, kTowerCmds[i].desc);
        XPLMRegisterCommandHandler(r, towerCmdHandler, 1, (void*)(intptr_t)i);
    }

    int idx = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "XRadio", nullptr, 0);
    g_menu = XPLMCreateMenu("XRadio", XPLMFindPluginsMenu(), idx, menuHandler, nullptr);
    XPLMAppendMenuItem(g_menu, "Show / hide window",   (void*)0, 0);
    XPLMAppendMenuItem(g_menu, "Settings...",          (void*)1, 0);
    XPLMAppendMenuItem(g_menu, "Reconnect",            (void*)2, 0);
    XPLMAppendMenuItem(g_menu, "Reset window position", (void*)3, 0);
    XPLMAppendMenuItem(g_menu, "Send test message",    (void*)4, 0);
    XPLMAppendMenuItem(g_menu, "Tower mode (TowerTrainer)", (void*)5, 0);
    XPLMAppendMenuItem(g_menu, "Tower: show / hide windows", (void*)6, 0);
    XPLMAppendMenuItem(g_menu, "Tower: view from the apron", (void*)7, 0);
    XPLMAppendMenuItem(g_menu, "Tower: back to the cab",    (void*)8, 0);

    XPLMCreateFlightLoop_t fl{};
    fl.structSize   = sizeof(fl);
    fl.phase        = xplm_FlightLoop_Phase_AfterFlightModel;
    fl.callbackFunc = flightLoop;
    g_loop = XPLMCreateFlightLoop(&fl);

    logMsg("started");
    return 1;
}

PLUGIN_API int XPluginEnable(void) {
    g_enabled = true;
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
    g_enabled = false;
    g_deferred.clear();                  // clicks made before disabling: a disabled plugin does nothing
    g_applyQueued = false;
    xr::tower::leave();
    xr::voice::setTransmitting(false);
    xr::voice::releaseMicrophone();      // a disabled plugin holds nothing
    netStop();
    sendLogout();
    g_sock.close();
    g_connected = false;
    g_sessionId.store(0);
    g_remote.clear();
    g_shadow.clear();
    xr::csl::disable();
    XPLMScheduleFlightLoop(g_loop, 0, 1);
}

PLUGIN_API void XPluginStop(void) {
#ifdef XRADIO_UI_IMGUI
    {   // remember where the window and the overlay were, and the tab
        const std::string g = xr::ui2::host::mainGeometry(), o = xr::ui2::host::overlayPosition();
        if (!g.empty()) g_cfg.uiWin = g;
        if (!o.empty()) g_cfg.uiOverlayPos = o;
        g_cfg.uiTab = std::to_string(xr::ui2::host::currentTab());
        g_cfg.towerLayout = xr::ui2::host::towerLayout();
        xr::saveSettings(g_cfg, g_cfgPath);
    }
    xr::ui2::host::shutdown();
#endif
    xr::simbrief::shutdown();
    xr::tower::shutdown();
    netStop();
    g_deferred.clear();
    g_applyQueued = false;
    if (g_hostWorker.joinable()) g_hostWorker.join();   // a server change still in hand finishes first
    g_hostResult = HostingResult{};
    g_reconnectAfterHosting = false;
    xr::relay::stop();
    xr::upnp::releaseAsync();      // give the router's port back if we can
    xr::upnp::shutdown();
    xr::update::shutdown();
    xr::voice::shutdown();
    xr::csl::shutdown();
    if (g_loop)   { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    if (g_window)      { XPLMDestroyWindow(g_window);      g_window = nullptr; }
    if (g_settingsWin) { XPLMDestroyWindow(g_settingsWin); g_settingsWin = nullptr; }
    if (g_noticeWin)   { XPLMDestroyWindow(g_noticeWin);   g_noticeWin = nullptr; }
    if (g_cmdPtt) { XPLMUnregisterCommandHandler(g_cmdPtt, pttHandler, 1, nullptr); }
    XPLMUnregisterKeySniffer(pttKeySniffer, 0, nullptr);
    if (g_menu)   { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int msg, void* param) {
    // the user's aircraft (index 0) or its livery changed: detect the type again on the next frame
    if ((msg == XPLM_MSG_PLANE_LOADED || msg == XPLM_MSG_LIVERY_LOADED) && (intptr_t)param == 0) g_acfDirty = true;
}
