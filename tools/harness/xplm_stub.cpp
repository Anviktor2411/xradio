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
std::vector<std::string>      g_drawn;
std::vector<std::string>      g_log;
XPLMFlightLoop_f              g_loopFunc   = nullptr;
XPLMDrawWindow_f              g_drawFunc   = nullptr;
XPLMCommandCallback_f         g_pttHandler = nullptr;
XPLMMenuHandler_f             g_menuFunc   = nullptr;
bool                          g_verbose    = false;

void set(const std::string& name, double v) { g_values[name] = v; }

double get(const std::string& name) {
    auto it = g_values.find(name);
    return it == g_values.end() ? 0.0 : it->second;
}

void tick(float dt) { if (g_loopFunc) g_loopFunc(dt, dt, 0, nullptr); }

std::vector<std::string> draw() {
    g_drawn.clear();
    if (g_drawFunc) g_drawFunc((XPLMWindowID)1, nullptr);
    return g_drawn;
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

int XPLMGetDatavf(XPLMDataRef r, float* out, int, int inMax) {
    if (inMax < 1 || !out) return 0;
    out[0] = (float)harness::get(refName(r));
    return 1;
}

XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t* p) {
    harness::g_loopFunc = p->callbackFunc;
    return (XPLMFlightLoopID)1;
}
void XPLMDestroyFlightLoop(XPLMFlightLoopID) { harness::g_loopFunc = nullptr; }
void XPLMScheduleFlightLoop(XPLMFlightLoopID, float, int) {}

XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t* p) {
    harness::g_drawFunc = p->drawWindowFunc;
    return (XPLMWindowID)1;
}
void XPLMDestroyWindow(XPLMWindowID) { harness::g_drawFunc = nullptr; }
void XPLMGetWindowGeometry(XPLMWindowID, int* l, int* t, int* r, int* b) {
    *l = 0; *t = 800; *r = 500; *b = 0;
}
void XPLMSetWindowTitle(XPLMWindowID, const char*) {}
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int) {}
void XPLMSetWindowIsVisible(XPLMWindowID, int) {}
int  XPLMGetWindowIsVisible(XPLMWindowID) { return 1; }
void XPLMGetScreenBoundsGlobal(int* l, int* t, int* r, int* b) {
    *l = 0; *t = 1080; *r = 1920; *b = 0;
}

void XPLMDrawString(float*, int, int, char* s, int*, XPLMFontID) {
    if (s) harness::g_drawn.push_back(s);
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
