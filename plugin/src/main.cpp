// XRadio -- X-Plane 12 multiplayer + radio plugin.
//
// Phase 1: log in to the relay server, send our own position 5x a second,
// receive nearby traffic and show it in a window. Aircraft rendering (XPMP2)
// and voice (Opus) plug into the hooks marked TODO below.

#define _USE_MATH_DEFINES   // MSVC needs this before <cmath> for M_PI

#include "net.h"
#include "protocol.h"
#include "xpmp_bridge.h"

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
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

void writeDefaultConfig() {
    FILE* f = fopen(g_cfgPath.c_str(), "w");
    if (!f) return;
    fprintf(f,
            "# XRadio configuration\n"
            "host = %s\n"
            "port = %d\n"
            "callsign = %s\n"
            "actype = %s\n",
            g_cfg.host.c_str(), g_cfg.port, g_cfg.callsign.c_str(), g_cfg.acIcao.c_str());
    fclose(f);
    logMsg("wrote default config to %s", g_cfgPath.c_str());
}

void loadConfig() {
    resolveConfigPath();
    FILE* f = fopen(g_cfgPath.c_str(), "r");
    if (!f) { writeDefaultConfig(); return; }

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

void findRefs() {
    g_ref.lat      = XPLMFindDataRef("sim/flightmodel/position/latitude");
    g_ref.lon      = XPLMFindDataRef("sim/flightmodel/position/longitude");
    g_ref.elev     = XPLMFindDataRef("sim/flightmodel/position/elevation");
    g_ref.psi      = XPLMFindDataRef("sim/flightmodel/position/psi");
    g_ref.theta    = XPLMFindDataRef("sim/flightmodel/position/theta");
    g_ref.phi      = XPLMFindDataRef("sim/flightmodel/position/phi");
    g_ref.gs       = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    g_ref.gear     = XPLMFindDataRef("sim/flightmodel2/gear/deploy_ratio");
    g_ref.flap     = XPLMFindDataRef("sim/cockpit2/controls/flap_ratio");
    g_ref.onGround = XPLMFindDataRef("sim/flightmodel/failures/onground_any");

    // 8.33 kHz variant reports the frequency in kHz, e.g. 118000 == 118.000 MHz
    g_ref.com1        = XPLMFindDataRef("sim/cockpit2/radios/actuators/com1_frequency_hz_833");
    g_ref.com2        = XPLMFindDataRef("sim/cockpit2/radios/actuators/com2_frequency_hz_833");
    g_ref.audioComSel = XPLMFindDataRef("sim/cockpit2/radios/actuators/audio_com_selection");
    g_ref.rxCom1      = XPLMFindDataRef("sim/cockpit2/radios/actuators/audio_selection_com1");
    g_ref.rxCom2      = XPLMFindDataRef("sim/cockpit2/radios/actuators/audio_selection_com2");

    g_ref.ltNav     = XPLMFindDataRef("sim/cockpit2/switches/navigation_lights_on");
    g_ref.ltBeacon  = XPLMFindDataRef("sim/cockpit2/switches/beacon_on");
    g_ref.ltStrobe  = XPLMFindDataRef("sim/cockpit2/switches/strobe_lights_on");
    g_ref.ltLanding = XPLMFindDataRef("sim/cockpit2/switches/landing_lights_on");
    g_ref.ltTaxi    = XPLMFindDataRef("sim/cockpit2/switches/taxi_light_on");
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

// ---------------------------------------------------------------------------
// plugin state
// ---------------------------------------------------------------------------
xr::UdpSocket    g_sock;
uint32_t         g_sessionId    = 0;
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
    h.sessionId  = g_sessionId;
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

    xr::TextHeader th{};
    th.freqKhz     = 0;                  // the server fills this from our TX radio
    th.fromSession = g_sessionId;
    strncpy(th.from, g_cfg.callsign.c_str(), sizeof(th.from) - 1);
    th.textLen     = len;
    memcpy(buf + off, &th, sizeof(th));
    memcpy(buf + off + sizeof(th), text.data(), len);
    g_sock.send(buf, off + (int)sizeof(th) + len);
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

        char cs[17] = {0}, ic[9] = {0};
        memcpy(cs, e.callsign, 16);
        memcpy(ic, e.acIcao, 8);

        Remote& r   = g_remote[e.sessionId];
        r.sid       = e.sessionId;
        r.callsign  = cs;
        r.acIcao    = ic;
        r.lat       = e.lat;
        r.lon       = e.lon;
        r.altMslM   = e.altMslM;
        r.heading   = e.headingTrue;
        r.pitch     = e.pitch;
        r.roll      = e.roll;
        r.gsMs      = e.gsMs;
        r.gear      = e.gearRatio;
        r.flap      = e.flapRatio;
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

    char from[17] = {0};
    memcpy(from, th.from, 16);
    std::string msg(reinterpret_cast<const char*>(payload + sizeof(th)), textLen);

    char line[320];
    snprintf(line, sizeof(line), "[%.3f] %s: %s", th.freqKhz / 1000.0, from, msg.c_str());
    addChat(line);
}

void pumpNetwork() {
    uint8_t buf[xr::kMaxPacket];
    for (int guard = 0; guard < 64; ++guard) {   // bounded, so one frame cannot stall
        int n = g_sock.recv(buf, sizeof(buf));
        if (n <= 0) break;
        if (n < (int)sizeof(xr::Header)) continue;

        xr::Header h{};
        memcpy(&h, buf, sizeof(h));
        if (h.magic != xr::kMagic || h.version != xr::kProtoVersion) continue;
        if ((int)(sizeof(h) + h.payloadLen) > n) continue;

        const uint8_t* payload = buf + sizeof(h);
        g_lastRxTime = g_elapsed;

        switch (h.type) {
            case xr::PT_LOGIN_ACK: {
                if (h.payloadLen < sizeof(xr::LoginAckPayload)) break;
                xr::LoginAckPayload a{};
                memcpy(&a, payload, sizeof(a));
                g_sessionId = a.sessionId;
                g_connected = true;
                g_status    = "connected to " + g_sock.endpoint();
                logMsg("connected, session %u", (unsigned)g_sessionId);
                break;
            }
            case xr::PT_TRAFFIC: handleTraffic(payload, h.payloadLen); break;
            case xr::PT_TEXT:    handleText(payload, h.payloadLen);    break;
            case xr::PT_VOICE:
                // TODO(voice): decode the Opus frame and feed the mixer.
                break;
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

    if (!g_connected) {
        if (g_elapsed - g_lastLogin > 2.0f) sendLogin();   // retry until acked
        return 0.2f;
    }

    // the server drops us after 15 s of silence, so position doubles as keepalive
    sendPosition();

    if (g_elapsed - g_lastRxTime > 12.0f) {
        g_connected = false;
        g_sessionId = 0;
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
    if (phase == xplm_CommandBegin)      g_pttDown = true;
    else if (phase == xplm_CommandEnd)   g_pttDown = false;
    // TODO(voice): start/stop microphone capture here.
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
    y -= 18;

    char hdr[128];
    snprintf(hdr, sizeof(hdr), "COM1 %.3f   COM2 %.3f   %s",
             id(g_ref.com1) / 1000.0, id(g_ref.com2) / 1000.0,
             g_pttDown ? "** TX **" : "");
    XPLMDrawString(g_pttDown ? green : white, x, y, hdr, nullptr, xplmFont_Proportional);
    y -= 22;

    char title[96];
    if (xr::csl::available()) {
        snprintf(title, sizeof(title), "Traffic (%d)  ·  %d CSL models",
                 (int)g_remote.size(), xr::csl::cslModelCount());
    } else {
        snprintf(title, sizeof(title), "Traffic (%d)  ·  no 3D models",
                 (int)g_remote.size());
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

void createWindow() {
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);

    XPLMCreateWindow_t p{};
    p.structSize            = sizeof(p);
    p.left                  = sl + 60;
    p.top                   = st - 100;
    p.right                 = sl + 520;
    p.bottom                = st - 500;
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

void menuHandler(void*, void* item) {
    intptr_t which = (intptr_t)item;
    if (which == 0 && g_window) {
        XPLMSetWindowIsVisible(g_window, !XPLMGetWindowIsVisible(g_window));
    } else if (which == 1) {
        loadConfig();
        g_connected = false;
        g_sessionId = 0;
        g_remote.clear();
        std::string err;
        g_sock.close();
        if (!g_sock.open(g_cfg.host, (uint16_t)g_cfg.port, &err)) {
            g_status = "socket error: " + err;
            logMsg("%s", g_status.c_str());
        } else {
            sendLogin();
        }
    } else if (which == 2) {
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

    std::string cslErr;
    if (!xr::csl::init(pluginRootDir(), g_cfg.acIcao, &cslErr)) {
        logMsg("3D traffic unavailable: %s", cslErr.c_str());
    }

    g_cmdPtt = XPLMCreateCommand("xradio/ptt", "XRadio: push to talk");
    XPLMRegisterCommandHandler(g_cmdPtt, pttHandler, 1, nullptr);

    int idx = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "XRadio", nullptr, 0);
    g_menu = XPLMCreateMenu("XRadio", XPLMFindPluginsMenu(), idx, menuHandler, nullptr);
    XPLMAppendMenuItem(g_menu, "Show / hide window", (void*)0, 0);
    XPLMAppendMenuItem(g_menu, "Reload config & reconnect", (void*)1, 0);
    XPLMAppendMenuItem(g_menu, "Send test message", (void*)2, 0);

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
    }
    XPLMScheduleFlightLoop(g_loop, 0.2f, 1);
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    if (g_sock.isOpen() && g_connected) {
        uint8_t buf[sizeof(xr::Header)];
        writeHeader(buf, xr::PT_LOGOUT, 0);
        g_sock.send(buf, (int)sizeof(buf));
    }
    g_sock.close();
    g_connected = false;
    g_sessionId = 0;
    g_remote.clear();
    xr::csl::disable();
    XPLMScheduleFlightLoop(g_loop, 0, 1);
}

PLUGIN_API void XPluginStop(void) {
    xr::csl::shutdown();
    if (g_loop)   { XPLMDestroyFlightLoop(g_loop); g_loop = nullptr; }
    if (g_window) { XPLMDestroyWindow(g_window);   g_window = nullptr; }
    if (g_cmdPtt) { XPLMUnregisterCommandHandler(g_cmdPtt, pttHandler, 1, nullptr); }
    if (g_menu)   { XPLMDestroyMenu(g_menu); g_menu = nullptr; }
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
