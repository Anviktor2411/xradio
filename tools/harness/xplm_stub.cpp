// A fake X-Plane, just real enough to run the plugin outside the sim.
//
// This implements the handful of XPLM functions main.cpp calls, so the actual
// plugin code -- the real socket, the real packet parsing, the real window
// drawing -- can be driven against a real server in CI. It is a test double,
// not a simulator: datarefs come from a table the harness sets, and "drawing"
// captures the strings the window would have shown.
#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h"

#include "harness.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <direct.h>      // _mkdir
#else
#  include <sys/stat.h>    // mkdir
#endif

namespace harness {

std::string tempDir() {
    static const std::string dir = [] {
#ifdef _WIN32
        const char* t = getenv("TEMP");
        if (!t || !*t) t = getenv("TMP");
        std::string base = (t && *t) ? t : "C:/Temp";
        for (auto& c : base) if (c == '\\') c = '/';
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/xradio-harness";
#else
        return std::string("/tmp/xradio-harness");
#endif
    }();
    return dir;
}

std::string cfgPath() { return tempDir() + "/xradio.cfg"; }

bool ensureTempDir() {
    const std::string d = tempDir();
#ifdef _WIN32
    const int rc = _mkdir(d.c_str());
#else
    const int rc = mkdir(d.c_str(), 0755);
#endif
    if (rc == 0) return true;
    // Already there is fine; anything else is not.
    FILE* probe = fopen((d + "/.probe").c_str(), "w");
    if (!probe) return false;
    fclose(probe);
    remove((d + "/.probe").c_str());
    return true;
}

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// The ground, for the terrain probe. Sea level and a working probe unless a
// test says otherwise.
double g_terrainM  = 0.0;
bool   g_probeWorks = true;
void setTerrain(double m)   { g_terrainM = m; }
void setProbeWorks(bool ok) { g_probeWorks = ok; }

std::map<std::string, double> g_values;
std::map<std::string, std::string> g_strings;
std::map<std::string, std::vector<float>> g_arrays;
std::vector<std::string>      g_drawn;
std::vector<Drawn>            g_drawnAt;   // same lines, with where they landed
std::vector<std::string>      g_log;
XPLMFlightLoop_f              g_loopFunc   = nullptr;
XPLMDrawWindow_f              g_drawFunc   = nullptr;   // first window (main)

struct Win {
    XPLMDrawWindow_f       draw  = nullptr;
    XPLMHandleMouseClick_f click = nullptr;
    XPLMHandleKey_f        key   = nullptr;
    int  l = 0, t = 800, r = 500, b = 0;
    bool visible = true;
};
std::vector<Win> g_wins;                  // index+1 is the XPLMWindowID
XPLMWindowID     g_focus = nullptr;

// Screen / monitor geometry the test can distort to reproduce odd setups.
int  g_screenL = 0, g_screenT = 1080, g_screenR = 1920, g_screenB = 0;
struct Mon { int l, t, r, b; };
std::vector<Mon> g_monitors;
XPLMCommandCallback_f         g_pttHandler = nullptr;
XPLMKeySniffer_f              g_sniffer    = nullptr;
void*                         g_snifferRef = nullptr;
XPLMMenuHandler_f             g_menuFunc   = nullptr;
bool                          g_verbose    = false;

void set(const std::string& name, double v) { g_values[name] = v; }
void setString(const std::string& name, const std::string& v) { g_strings[name] = v; }
std::string getString(const std::string& name) {
    auto it = g_strings.find(name);
    return it == g_strings.end() ? std::string() : it->second;
}

double get(const std::string& name) {
    auto it = g_values.find(name);
    return it == g_values.end() ? 0.0 : it->second;
}

void setArray(const std::string& name, const std::vector<float>& v) { g_arrays[name] = v; }

const std::vector<float>& getArray(const std::string& name) {
    static const std::vector<float> empty;
    auto it = g_arrays.find(name);
    return it == g_arrays.end() ? empty : it->second;
}

void tick(float dt) { if (g_loopFunc) g_loopFunc(dt, dt, 0, nullptr); }

std::vector<std::string> draw() {
    g_drawn.clear();
    g_drawnAt.clear();
    if (g_drawFunc) g_drawFunc((XPLMWindowID)1, nullptr);
    return g_drawn;
}

// --- second window (settings) helpers ------------------------------------
static Win* win(int id) {
    return (id >= 1 && id <= (int)g_wins.size()) ? &g_wins[id - 1] : nullptr;
}

std::vector<std::string> drawWindow(int id) {
    g_drawn.clear();
    g_drawnAt.clear();
    if (Win* w = win(id); w && w->draw) w->draw((XPLMWindowID)(intptr_t)id, nullptr);
    return g_drawn;
}

// Where the most recent draw put a line. Lets a test click the row a label is
// actually on instead of re-deriving the layout and drifting out of sync.
bool drawnAt(const std::string& needle, int* x, int* y) {
    for (const auto& d : g_drawnAt) {
        if (d.text.find(needle) == std::string::npos) continue;
        if (x) *x = d.x;
        if (y) *y = d.y;
        return true;
    }
    return false;
}

const std::vector<Drawn>& drawnPositions() { return g_drawnAt; }

bool windowVisible(int id) { Win* w = win(id); return w && w->visible; }

void click(int id, int x, int y) {
    Win* w = win(id);
    if (!w || !w->click) return;
    w->click((XPLMWindowID)(intptr_t)id, x, y, xplm_MouseDown, nullptr);
    w->click((XPLMWindowID)(intptr_t)id, x, y, xplm_MouseUp, nullptr);
}

void typeText(int id, const std::string& text) {
    Win* w = win(id);
    if (!w || !w->key) return;
    for (char c : text) {
        w->key((XPLMWindowID)(intptr_t)id, c, xplm_DownFlag, 0, nullptr, 0);
        w->key((XPLMWindowID)(intptr_t)id, c, xplm_UpFlag,   0, nullptr, 0);
    }
}

void pressVk(int id, int vk) {
    Win* w = win(id);
    if (!w || !w->key) return;
    w->key((XPLMWindowID)(intptr_t)id, 0, xplm_DownFlag, (char)vk, nullptr, 0);
    w->key((XPLMWindowID)(intptr_t)id, 0, xplm_UpFlag,   (char)vk, nullptr, 0);
}

// Forget every window, so a test can run XPluginStart more than once
// without inspecting a previous run's geometry by mistake.
void resetWindows() {
    g_wins.clear();
    g_drawFunc = nullptr;
    g_focus = nullptr;
}

void setScreen(int l, int t, int r, int b) {
    g_screenL = l; g_screenT = t; g_screenR = r; g_screenB = b;
}
void clearMonitors() { g_monitors.clear(); }
void addMonitor(int l, int t, int r, int b) { g_monitors.push_back({l, t, r, b}); }

// Drag a window to a new size, as a pilot would.
void setWindowRect(int id, int l, int t, int r, int b) {
    if (Win* w = win(id)) { w->l = l; w->t = t; w->r = r; w->b = b; }
}

void windowRect(int id, int* l, int* t, int* r, int* b) {
    Win* w = win(id);
    if (l) *l = w ? w->l : 0;
    if (t) *t = w ? w->t : 0;
    if (r) *r = w ? w->r : 0;
    if (b) *b = w ? w->b : 0;
}

void windowTop(int id, int* t, int* l) {
    Win* w = win(id);
    if (t) *t = w ? w->t : 0;
    if (l) *l = w ? w->l : 0;
}

// A key pressed in the sim with no window focused: goes to the key sniffer.
// Returns what the sniffer returned (0 = it ate the key), or 1 if none.
int simKey(int vk, bool down) {
    if (!g_sniffer) return 1;
    return g_sniffer(0, down ? xplm_DownFlag : xplm_UpFlag, (char)vk, g_snifferRef);
}

void ptt(bool down) {
    if (g_pttHandler) {
        g_pttHandler((XPLMCommandRef)1, down ? xplm_CommandBegin : xplm_CommandEnd, nullptr);
    }
}

void menu(int item) {
    if (g_menuFunc) g_menuFunc(nullptr, (void*)(intptr_t)item);
}

bool drawnContains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

const std::vector<std::string>& log() { return g_log; }

}  // namespace harness

// --- the fake SDK -----------------------------------------------------------
// Dataref "handles" are just interned name pointers.
static std::map<std::string, std::string>* g_names = nullptr;

// Names the stub deliberately does not know about, so the plugin's
// missing-dataref reporting can be tested.
static const char* kUnknown[] = {nullptr};

XPLMDataRef XPLMFindDataRef(const char* name) {
    if (!g_names) g_names = new std::map<std::string, std::string>();
    for (int i = 0; kUnknown[i]; ++i) {
        if (!strcmp(kUnknown[i], name)) return nullptr;
    }
    auto& slot = (*g_names)[name];
    slot = name;
    return (XPLMDataRef)&slot;
}

static std::string refName(XPLMDataRef r) {
    return r ? *(const std::string*)r : std::string();
}

float  XPLMGetDataf(XPLMDataRef r) { return (float)harness::get(refName(r)); }
double XPLMGetDatad(XPLMDataRef r) { return harness::get(refName(r)); }
int    XPLMGetDatai(XPLMDataRef r) { return (int)harness::get(refName(r)); }

// String datarefs (acf_ICAO, livery path): the harness sets them with
// setString(); a numeric-only ref reads back as empty.
int XPLMGetDatab(XPLMDataRef r, void* out, int, int inMax) {
    const std::string v = harness::getString(refName(r));
    if (!out) return (int)v.size();
    const int n = (int)v.size() < inMax ? (int)v.size() : inMax;
    memcpy(out, v.data(), (size_t)n);
    return n;
}

// Array datarefs (the thirteen wind layers, the three cloud layers). Stored
// per name by the harness; a ref that was only ever set as a scalar reads
// back as its single value, which is what X-Plane does for a one-element
// array too.
int XPLMGetDatavf(XPLMDataRef r, float* out, int inOffset, int inMax) {
    if (inMax < 1 || !out) return 0;
    const std::vector<float>& v = harness::getArray(refName(r));
    if (v.empty()) {
        out[0] = (float)harness::get(refName(r));
        return 1;
    }
    int n = 0;
    for (int i = inOffset; i < (int)v.size() && n < inMax; ++i, ++n) out[n] = v[(size_t)i];
    return n;
}

void XPLMSetDatavf(XPLMDataRef r, float* in, int inOffset, int inCount) {
    if (!in || inCount < 1) return;
    std::vector<float> v = harness::getArray(refName(r));
    if ((int)v.size() < inOffset + inCount) v.resize((size_t)(inOffset + inCount), 0.f);
    for (int i = 0; i < inCount; ++i) v[(size_t)(inOffset + i)] = in[i];
    harness::setArray(refName(r), v);
}

void XPLMSetDataf(XPLMDataRef r, float v) { harness::set(refName(r), v); }
void XPLMSetDatai(XPLMDataRef r, int v)   { harness::set(refName(r), v); }

XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t* p) {
    harness::g_loopFunc = p->callbackFunc;
    return (XPLMFlightLoopID)1;
}
void XPLMDestroyFlightLoop(XPLMFlightLoopID) { harness::g_loopFunc = nullptr; }
void XPLMScheduleFlightLoop(XPLMFlightLoopID, float, int) {}

XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* p) {
    harness::Win w;
    w.draw = p->drawWindowFunc;
    w.click = p->handleMouseClickFunc;
    w.key = p->handleKeyFunc;
    w.l = p->left; w.t = p->top; w.r = p->right; w.b = p->bottom;
    w.visible = p->visible != 0;
    harness::g_wins.push_back(w);
    if (harness::g_wins.size() == 1) harness::g_drawFunc = p->drawWindowFunc;
    return (XPLMWindowID)(intptr_t)harness::g_wins.size();
}
void XPLMDestroyWindow(XPLMWindowID id) {
    if ((intptr_t)id == 1) harness::g_drawFunc = nullptr;
}
void XPLMGetWindowGeometry(XPLMWindowID id, int* l, int* t, int* r, int* b) {
    auto* w = ((intptr_t)id >= 1 && (intptr_t)id <= (intptr_t)harness::g_wins.size())
              ? &harness::g_wins[(intptr_t)id - 1] : nullptr;
    *l = w ? w->l : 0; *t = w ? w->t : 800; *r = w ? w->r : 500; *b = w ? w->b : 0;
}
void XPLMSetWindowTitle(XPLMWindowID, const char*) {}
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int) {}
void XPLMSetWindowIsVisible(XPLMWindowID id, int v) {
    if ((intptr_t)id >= 1 && (intptr_t)id <= (intptr_t)harness::g_wins.size())
        harness::g_wins[(intptr_t)id - 1].visible = v != 0;
}
int  XPLMGetWindowIsVisible(XPLMWindowID id) {
    if ((intptr_t)id >= 1 && (intptr_t)id <= (intptr_t)harness::g_wins.size())
        return harness::g_wins[(intptr_t)id - 1].visible ? 1 : 0;
    return 0;
}
void XPLMBringWindowToFront(XPLMWindowID) {}
void XPLMTakeKeyboardFocus(XPLMWindowID id) { harness::g_focus = id; }
int  XPLMHasKeyboardFocus(XPLMWindowID id) { return harness::g_focus == id ? 1 : 0; }
void XPLMGetScreenBoundsGlobal(int* l, int* t, int* r, int* b) {
    *l = harness::g_screenL; *t = harness::g_screenT;
    *r = harness::g_screenR; *b = harness::g_screenB;
}
void XPLMGetAllMonitorBoundsGlobal(XPLMReceiveMonitorBoundsGlobal_f cb, void* ref) {
    int i = 0;
    for (auto& m : harness::g_monitors) cb(i++, m.l, m.t, m.r, m.b, ref);
}
void XPLMSetWindowGeometry(XPLMWindowID id, int l, int t, int r, int b) {
    if ((intptr_t)id >= 1 && (intptr_t)id <= (intptr_t)harness::g_wins.size()) {
        auto& w = harness::g_wins[(intptr_t)id - 1];
        w.l = l; w.t = t; w.r = r; w.b = b;
    }
}

void XPLMDrawString(float*, int x, int y, char* s, int*, XPLMFontID) {
    if (!s) return;
    harness::g_drawn.push_back(s);
    harness::g_drawnAt.push_back({s, x, y});
}

float XPLMMeasureString(XPLMFontID, const char* s, int n) {
    (void)s;
    return 7.f * (float)(n < 0 ? 0 : n);      // the fixed-pitch font's width
}

// X-Plane's fixed-pitch font is 7 x 10; the UI lays text out from these.
void XPLMGetFontDimensions(XPLMFontID, int* w, int* h, int* digitsOnly) {
    if (w) *w = 7;
    if (h) *h = 10;
    if (digitsOnly) *digitsOnly = 0;
}

int XPLMRegisterKeySniffer(XPLMKeySniffer_f cb, int, void* ref) {
    harness::g_sniffer = cb;
    harness::g_snifferRef = ref;
    return 1;
}
int XPLMUnregisterKeySniffer(XPLMKeySniffer_f, int, void*) {
    harness::g_sniffer = nullptr;
    return 1;
}

XPLMMenuID XPLMFindPluginsMenu(void) { return (XPLMMenuID)1; }
int XPLMAppendMenuItem(XPLMMenuID, const char*, void*, int) { return 0; }
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f h, void*) {
    harness::g_menuFunc = h;
    return (XPLMMenuID)2;
}
void XPLMDestroyMenu(XPLMMenuID) {}

void XPLMDebugString(const char* s) {
    if (s) {
        harness::g_log.push_back(s);
        if (harness::g_verbose) fputs(s, stderr);
    }
}

// What the sim would say out loud, kept so a test can check it was asked to.
void XPLMSpeakString(const char* s) {
    harness::g_log.push_back(std::string("speak: ") + (s ? s : ""));
}

void XPLMGetPrefsPath(char* out) {
    snprintf(out, 1024, "%s/prefs.txt", harness::tempDir().c_str());
}
const char* XPLMGetDirectorySeparator(void) { return "/"; }

void XPLMExtractFileAndPath(char* path) {
    char* slash = strrchr(path, '/');
    if (slash) *slash = '\0';
}

int XPLMEnableFeature(const char*, int) { return 1; }

namespace harness {
std::map<std::string, int>            g_cmdIds;        // name -> ref id (1 = xradio/ptt, created first)
std::map<int, XPLMCommandCallback_f>  g_cmdHandlers;
std::map<int, void*>                  g_cmdRefcons;
int command(const std::string& name, int phase) {
    auto it = g_cmdIds.find(name);
    if (it == g_cmdIds.end()) return -1;
    auto h = g_cmdHandlers.find(it->second);
    if (h == g_cmdHandlers.end() || !h->second) return -1;
    return h->second((XPLMCommandRef)(intptr_t)it->second, (XPLMCommandPhase)phase, g_cmdRefcons[it->second]);
}
}  // namespace harness
XPLMCommandRef XPLMCreateCommand(const char* name, const char*) {
    auto it = harness::g_cmdIds.find(name ? name : "");
    if (it != harness::g_cmdIds.end()) return (XPLMCommandRef)(intptr_t)it->second;
    const int id = (int)harness::g_cmdIds.size() + 1;
    harness::g_cmdIds[name ? name : ""] = id;
    return (XPLMCommandRef)(intptr_t)id;
}
void XPLMRegisterCommandHandler(XPLMCommandRef r, XPLMCommandCallback_f h, int, void* ref) {
    const int id = (int)(intptr_t)r;
    harness::g_cmdHandlers[id] = h;
    harness::g_cmdRefcons[id] = ref;
    for (const auto& kv : harness::g_cmdIds) if (kv.second == id && kv.first == "xradio/ptt") harness::g_pttHandler = h;
}
void XPLMUnregisterCommandHandler(XPLMCommandRef, XPLMCommandCallback_f, int, void*) {}

XPLMPluginID XPLMGetMyID(void) { return 1; }
void XPLMGetPluginInfo(XPLMPluginID, char*, char* outFilePath, char*, char*) {
    if (outFilePath)
        snprintf(outFilePath, 1024, "%s/XRadio/lin_x64/lin.xpl", harness::tempDir().c_str());
}

// ---------------------------------------------------------------------------
// Scenery: a flat earth in metres, so the plugin's terrain probe has
// something to answer with. Only the round trip matters to the caller --
// world -> local -> probe -> local -> world -- so the projection can be as
// crude as this as long as it is its own inverse.
static const double kDegM = 111320.0;

void XPLMWorldToLocal(double lat, double lon, double alt,
                      double* x, double* y, double* z) {
    if (x) *x =  lon * kDegM;
    if (y) *y =  alt;
    if (z) *z = -lat * kDegM;
}

void XPLMLocalToWorld(double x, double y, double z,
                      double* lat, double* lon, double* alt) {
    if (lat) *lat = -z / kDegM;
    if (lon) *lon =  x / kDegM;
    if (alt) *alt =  y;
}

XPLMProbeRef XPLMCreateProbe(XPLMProbeType) { return (XPLMProbeRef)1; }
void XPLMDestroyProbe(XPLMProbeRef) {}

XPLMProbeResult XPLMProbeTerrainXYZ(XPLMProbeRef probe, float x, float, float z,
                                    XPLMProbeInfo_t* out) {
    if (!probe || !out) return xplm_ProbeError;
    if (!harness::g_probeWorks) return xplm_ProbeMissed;
    out->locationX = x;
    out->locationY = (float)harness::g_terrainM;
    out->locationZ = z;
    out->normalY   = 1.f;
    out->is_wet    = 0;
    return xplm_ProbeHitTerrain;
}

// ---------------------------------------------------------------- Tower mode (v0.6.0)
#include "XPLMCamera.h"
namespace harness {
XPLMCameraControl_f  g_camFunc = nullptr;
void*                g_camRef  = nullptr;
XPLMCameraPosition_t g_camPos{};
bool cameraControlled() { return g_camFunc != nullptr; }
XPLMCameraPosition_t camera() {
    if (g_camFunc) { XPLMCameraPosition_t p{}; if (g_camFunc(&p, 0, g_camRef)) g_camPos = p; }
    return g_camPos;
}
}  // namespace harness

void XPLMControlCamera(XPLMCameraControlDuration, XPLMCameraControl_f f, void* ref) { harness::g_camFunc = f; harness::g_camRef = ref; }
void XPLMDontControlCamera(void) {
    if (harness::g_camFunc) { XPLMCameraPosition_t p{}; harness::g_camFunc(&p, 1, harness::g_camRef); }
    harness::g_camFunc = nullptr; harness::g_camRef = nullptr;
}
int  XPLMIsCameraBeingControlled(XPLMCameraControlDuration* d) { if (d) *d = xplm_ControlCameraUntilViewChanges; return harness::g_camFunc ? 1 : 0; }
void XPLMReadCameraPosition(XPLMCameraPosition_t* out) { if (out) *out = harness::camera(); }
void XPLMSetDatad(XPLMDataRef r, double v) { harness::set(refName(r), v); }
int  XPLMGetDatavi(XPLMDataRef r, int* out, int inOffset, int inMax) {
    const std::vector<float>& v = harness::getArray(refName(r));
    if (!out) return (int)v.size();
    int n = 0;
    for (int i = 0; i < inMax && inOffset + i < (int)v.size(); ++i) { out[i] = (int)v[(size_t)(inOffset + i)]; ++n; }
    return n;
}
void XPLMSetDatavi(XPLMDataRef r, int* in, int inOffset, int inCount) {
    std::vector<float> v = harness::getArray(refName(r));
    if ((int)v.size() < inOffset + inCount) v.resize((size_t)(inOffset + inCount), 0.f);
    for (int i = 0; i < inCount; ++i) v[(size_t)(inOffset + i)] = (float)in[i];
    harness::setArray(refName(r), v);
}
void XPLMGetSystemPath(char* out) { if (out) { const std::string d = harness::tempDir() + "/"; strncpy(out, d.c_str(), 511); out[511] = 0; } }
#include "XPLMPlanes.h"
// The .acf the user flies: the test sets "harness/acf_path" (empty = the default Cessna's path).
void XPLMGetNthAircraftModel(int, char* outFileName, char* outPath) {
    std::string p = harness::getString("harness/acf_path");
    if (p.empty()) p = "Aircraft/Laminar Research/Cessna 172 SP/Cessna_172SP.acf";
    const size_t slash = p.find_last_of("/\\");
    if (outFileName) { strncpy(outFileName, (slash == std::string::npos ? p : p.substr(slash + 1)).c_str(), 255); outFileName[255] = 0; }
    if (outPath) { strncpy(outPath, p.c_str(), 511); outPath[511] = 0; }
}
