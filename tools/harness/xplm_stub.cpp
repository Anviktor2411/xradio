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
#include "XPLMUtilities.h"

#include "harness.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace harness {

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

void XPLMGetPrefsPath(char* out) { strcpy(out, "/tmp/xradio-harness/prefs.txt"); }
const char* XPLMGetDirectorySeparator(void) { return "/"; }

void XPLMExtractFileAndPath(char* path) {
    char* slash = strrchr(path, '/');
    if (slash) *slash = '\0';
}

int XPLMEnableFeature(const char*, int) { return 1; }

XPLMCommandRef XPLMCreateCommand(const char*, const char*) { return (XPLMCommandRef)1; }
void XPLMRegisterCommandHandler(XPLMCommandRef, XPLMCommandCallback_f h, int, void*) {
    harness::g_pttHandler = h;
}
void XPLMUnregisterCommandHandler(XPLMCommandRef, XPLMCommandCallback_f, int, void*) {}

XPLMPluginID XPLMGetMyID(void) { return 1; }
void XPLMGetPluginInfo(XPLMPluginID, char*, char* outFilePath, char*, char*) {
    if (outFilePath) strcpy(outFilePath, "/tmp/xradio-harness/XRadio/lin_x64/lin.xpl");
}
