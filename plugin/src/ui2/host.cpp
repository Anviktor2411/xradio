#include "host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"

#include "app_ui.h"
#include "i18n.h"
#include "imgui_xp.h"
#include "theme.h"
#include "tower_model.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace xr {
namespace ui2 {
namespace host {

namespace {
std::unique_ptr<XpWindow> g_main, g_overlay;
Model   g_model;
Actions g_actions;
std::string g_theme = "dark";
float   g_scale = 1.f;
bool    g_overlayOn = true;
bool    g_themeApplied = false;
int     g_themeGen = 1;           // bumped when the theme or the scale changes: each window re-applies it in its context
XPLMDataRef g_vr = nullptr;
bool    g_wasVr = false;
double  g_readUntil = 0;

// Tower mode's windows
std::unique_ptr<XpWindow> g_tw[kTwCount];
TowerActions        g_twActions;
const tower::State* g_twState = nullptr;
TowerFrame          g_twFrame;
bool    g_twActive = false, g_twOn = false;
bool    g_twShown[kTwCount] = {};          // visible because we made it so
bool    g_twClosed[kTwCount] = {};         // closed with its close box: stays shut until the windows are opened again
int     g_twThemeGen[kTwCount] = {};
int     g_twGeom[kTwCount][4] = {};        // where each was last seen inside X-Plane, for the settings file
bool    g_twHaveGeom[kTwCount] = {};
tower::Phase g_twPhase = tower::Phase::Off;

Fonts uiFonts() {
    Fonts f; f.body = fonts().body; f.heading = fonts().heading; f.big = fonts().big; f.mono = fonts().mono; f.small = fonts().small;
    return f;
}

void parseGeom(const std::string& s, int* l, int* t, int* r, int* b) {
    int a[4] = {0, 0, 0, 0};
    if (sscanf(s.c_str(), "%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3]) == 4 && a[2] - a[0] > 200 && a[1] - a[3] > 150) { *l = a[0]; *t = a[1]; *r = a[2]; *b = a[3]; }
}

void defaultMain(int* l, int* t, int* r, int* b) {
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const int w = 760, h = 520;
    *l = sl + 80; *t = st - 80; *r = *l + w; *b = *t - h;
}

void drawMainWin(XpWindow& w) {
    (void)w;
    if (!g_themeApplied) { applyTheme(g_theme.c_str(), g_scale); g_themeApplied = true; }
    setFonts(uiFonts());
    ImGui::PushFont(fonts().body);
    g_model.readUntil = g_readUntil;
    drawMain(g_model, g_actions);
    g_readUntil = g_model.readUntil;
    ImGui::PopFont();
}

void drawOverlayWin(XpWindow& w) {
    setFonts(uiFonts());
    ImGui::PushFont(fonts().body);
    g_model.readUntil = g_readUntil;
    drawOverlay(g_model, g_actions, w.width());
    ImGui::PopFont();
    w.setPassThrough(!overlayWantsMouse());
}

// Where the tower windows open the first time: radar top left, strips top right, Delivery under the radar, the
// frequency bottom left, operations under the strips, the banner at the top in the middle.
void defaultTowerRect(int which, int* l, int* t, int* r, int* b) {
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const int W = std::max(900, sr - sl), H = std::max(600, st - sb);
    const int colL = std::min(620, W / 2 - 20), colR = std::min(560, W / 2 - 20), topH = std::min(360, H / 2 - 50);
    switch (which) {
        case kTwRadar:     *l = sl + 10; *t = st - 40; *r = *l + colL; *b = *t - topH; break;
        case kTwStrips:    *r = sr - 10; *t = st - 40; *l = *r - colR; *b = *t - std::min(340, H / 2 - 50); break;
        case kTwDelivery:  *l = sl + 10; *t = st - 50 - topH; *r = *l + colL; *b = sb + 250; break;
        case kTwFrequency: *l = sl + 10; *t = sb + 240; *r = *l + std::min(680, W / 2); *b = sb + 10; break;
        case kTwOps:       *r = sr - 10; *t = st - 50 - std::min(340, H / 2 - 50); *l = *r - colR; *b = sb + 10; break;
        default:           *l = sl + (W - 620) / 2; *t = st - 60; *r = *l + 620; *b = *t - 128; break;
    }
    if (*t - *b < 120) *b = *t - 120;
}

void drawTowerWin(int which, XpWindow& w) {
    if (g_twThemeGen[which] != g_themeGen) { applyTheme(g_theme.c_str(), g_scale); g_twThemeGen[which] = g_themeGen; }
    if (!g_twState) return;
    setTowerFonts(uiFonts());
    ImGui::PushFont(fonts().body);
    TowerFrame f = g_twFrame;
    f.which = which; f.poppedOut = w.poppedOut(); f.inVR = w.inVR();
    drawTower(f, *g_twState, g_twActions);
    ImGui::PopFont();
}
}  // namespace

void init(const Actions& actions, const Settings& settings) {
    g_actions = actions;
    g_theme = settings.uiTheme;
    g_scale = settings.uiScale > 0.5f ? settings.uiScale : 1.f;
    g_overlayOn = settings.uiOverlay;
    setLanguage(settings.uiLang);
    setUiScale(g_scale);
    rebuildFonts(g_scale);
    int l, t, r, b;
    defaultMain(&l, &t, &r, &b);
    parseGeom(settings.uiWin, &l, &t, &r, &b);
    g_main.reset(new XpWindow("XRadio", l, t, r, b, true, drawMainWin));
    g_main->setResizingLimits(560, 360, 2400, 1600);
    int ol = l, ot = t + 60;
    { int a = 0, c = 0; if (sscanf(settings.uiOverlayPos.c_str(), "%d,%d", &a, &c) == 2 && a >= 0 && c > 40) { ol = a; ot = c; } }
    g_overlay.reset(new XpWindow("XRadio overlay", ol, ot, ol + 520, ot - 64, false, drawOverlayWin));
    g_overlay->setResizingLimits(260, 40, 1200, 200);
    g_overlay->setPassThrough(true);
    g_overlay->setVisible(g_overlayOn);
    state().tab = atoi(settings.uiTab.c_str());
    g_vr = XPLMFindDataRef("sim/graphics/VR/enabled");
    // the first run shows the window with the wizard
    if (!settings.uiWizardDone) { g_main->setVisible(true); state().wizardOpen = true; }
}

void shutdown() {
    for (auto& w : g_tw) w.reset();
    g_twState = nullptr;
    g_overlay.reset();
    g_main.reset();
}

void update(const Model& model) {
    g_model = model;
    g_model.readUntil = g_readUntil;
    // VR: the windows follow the headset when VR starts, and come back when it stops
    const bool vr = g_vr && XPLMGetDatai(g_vr) != 0;
    if (vr != g_wasVr) {
        g_wasVr = vr;
        if (g_main) g_main->toVR(vr);
        if (g_overlay) g_overlay->toVR(vr);
        for (auto& w : g_tw) if (w) w->toVR(vr);
    }
}

void toggleMain() { if (g_main) { g_main->setVisible(!g_main->visible()); if (g_main->visible()) g_main->bringToFront(); } }
void showMain(bool on) { if (g_main) { g_main->setVisible(on); if (on) g_main->bringToFront(); } }
bool mainVisible() { return g_main && g_main->visible(); }
void showSettings() { state().tab = 6; state().wizardOpen = false; showMain(true); }
void toast(const std::string& text, int colour) { ui2::toast(text, colour, g_model.elapsed); }
int  currentTab() { return state().tab; }

void applyDisplay(const Settings& s) {
    const bool scaleChanged = s.uiScale > 0.5f && s.uiScale != g_scale;
    g_theme = s.uiTheme;
    g_overlayOn = s.uiOverlay;
    setLanguage(s.uiLang);
    if (scaleChanged) { g_scale = s.uiScale; setUiScale(g_scale); rebuildFonts(g_scale); }
    g_themeApplied = false;    // re-applied inside the next frame of each window (needs its context)
    ++g_themeGen;
    if (g_main) { ImGui::SetCurrentContext(g_main->context()); applyTheme(g_theme.c_str(), g_scale); }
    if (g_overlay) { ImGui::SetCurrentContext(g_overlay->context()); applyTheme(g_theme.c_str(), g_scale); g_overlay->setVisible(g_overlayOn); }
    g_themeApplied = true;
}

void resetPositions() {
    if (!g_main) return;
    int l, t, r, b;
    defaultMain(&l, &t, &r, &b);
    g_main->popOut(false);
    g_main->setGeometry(l, t, r, b);
    if (g_overlay) g_overlay->setGeometry(l, t + 60, l + 520, t - 4);
}

std::string mainGeometry() {
    if (!g_main || g_main->poppedOut() || g_main->inVR()) return "";
    int l, t, r, b; g_main->geometry(&l, &t, &r, &b);
    char s[64]; snprintf(s, sizeof s, "%d,%d,%d,%d", l, t, r, b);
    return s;
}

std::string overlayPosition() {
    if (!g_overlay || g_overlay->poppedOut() || g_overlay->inVR()) return "";
    int l, t, r, b; g_overlay->geometry(&l, &t, &r, &b);
    char s[32]; snprintf(s, sizeof s, "%d,%d", l, t);
    return s;
}

// ---------------------------------------------------------------- Tower mode's windows
void initTower(const TowerActions& actions, const std::string& layout) {
    g_twActions = actions;
    g_twActions.popOut = [](int which, bool out) { if (which >= 0 && which < kTwCount && g_tw[which]) g_tw[which]->popOut(out); };
    // the saved layout: "l,t,r,b|l,t,r,b|..." in window order; a missing or squashed one opens where it would by default
    std::vector<std::string> saved;
    for (size_t a = 0; a <= layout.size();) {
        const size_t e = layout.find('|', a);
        saved.push_back(layout.substr(a, e == std::string::npos ? std::string::npos : e - a));
        if (e == std::string::npos) break;
        a = e + 1;
    }
    static const int minSize[kTwCount][2] = {{380, 240}, {320, 160}, {460, 140}, {400, 180}, {440, 230}, {440, 96}};
    for (int i = 0; i < kTwCount; ++i) {
        int l, t, r, b;
        defaultTowerRect(i, &l, &t, &r, &b);
        int g[4] = {0, 0, 0, 0};
        if (i < (int)saved.size() && sscanf(saved[(size_t)i].c_str(), "%d,%d,%d,%d", &g[0], &g[1], &g[2], &g[3]) == 4 &&
            g[2] - g[0] >= minSize[i][0] && g[1] - g[3] >= minSize[i][1]) { l = g[0]; t = g[1]; r = g[2]; b = g[3]; }
        g_tw[i].reset(new XpWindow(tr(towerWindowTitle(i)), l, t, r, b, true, [i](XpWindow& w) { drawTowerWin(i, w); }));
        g_tw[i]->setResizingLimits(minSize[i][0], minSize[i][1], 2400, 1600);
        g_twGeom[i][0] = l; g_twGeom[i][1] = t; g_twGeom[i][2] = r; g_twGeom[i][3] = b;
        g_twHaveGeom[i] = true;
    }
}

void updateTower(const tower::State& s, bool active, bool windowsOn, const std::string& code, const std::string& position) {
    g_twState = &s;
    g_twFrame.code = code;
    g_twFrame.position = position;
    if (!g_tw[0]) return;
    // entering the tower, or opening the windows again, brings back windows closed with their close box
    if (active != g_twActive || (windowsOn && !g_twOn)) for (auto& c : g_twClosed) c = false;
    g_twActive = active;
    g_twOn = windowsOn;
    if (s.phase != g_twPhase) { g_twClosed[kTwBanner] = false; g_twPhase = s.phase; }   // something new to say
    const tw::Banner banner = tw::bannerFor(s, code);
    const bool ready = active && s.haveAirport && (s.phase == tower::Phase::Linked || s.phase == tower::Phase::HostAway);
    for (int i = 0; i < kTwCount; ++i) {
        XpWindow& w = *g_tw[i];
        const bool vis = w.visible();
        if (g_twShown[i] && !vis) g_twClosed[i] = true;
        const bool want = !g_twClosed[i] && (i == kTwBanner ? active && banner.show : ready && windowsOn);
        if (want != vis) { w.setVisible(want); if (want) w.bringToFront(); }
        g_twShown[i] = want;
        if (want && !w.poppedOut() && !w.inVR()) {
            int l, t, r, b;
            w.geometry(&l, &t, &r, &b);
            if (r - l > 100 && t - b > 60) { g_twGeom[i][0] = l; g_twGeom[i][1] = t; g_twGeom[i][2] = r; g_twGeom[i][3] = b; g_twHaveGeom[i] = true; }
        }
    }
    TowerUi& u = towerUi();
    if (u.wantFrequencyFront) {
        u.wantFrequencyFront = false;
        if (g_tw[kTwFrequency]->visible()) g_tw[kTwFrequency]->bringToFront();
    }
}

std::string towerLayout() {
    std::string out;
    for (int i = 0; i < kTwCount; ++i) {
        int l = g_twGeom[i][0], t = g_twGeom[i][1], r = g_twGeom[i][2], b = g_twGeom[i][3];
        if (!g_twHaveGeom[i]) defaultTowerRect(i, &l, &t, &r, &b);
        char s[64];
        snprintf(s, sizeof s, "%s%d,%d,%d,%d", i ? "|" : "", l, t, r, b);
        out += s;
    }
    return out;
}

void resetTowerPositions() {
    for (int i = 0; i < kTwCount; ++i) {
        if (!g_tw[i]) continue;
        int l, t, r, b;
        defaultTowerRect(i, &l, &t, &r, &b);
        g_tw[i]->popOut(false);
        g_tw[i]->setGeometry(l, t, r, b);
        g_twGeom[i][0] = l; g_twGeom[i][1] = t; g_twGeom[i][2] = r; g_twGeom[i][3] = b;
    }
}

}  // namespace host

void openUrlNative(const std::string& url) {
    if (url.rfind("http", 0) != 0) return;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + url + "' &"; (void)system(cmd.c_str());
#else
    std::string cmd = "xdg-open '" + url + "' &"; (void)system(cmd.c_str());
#endif
}

}  // namespace ui2
}  // namespace xr
