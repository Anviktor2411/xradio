#include "host.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"

#include "app_ui.h"
#include "i18n.h"
#include "imgui_xp.h"
#include "theme.h"

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
XPLMDataRef g_vr = nullptr;
bool    g_wasVr = false;
double  g_readUntil = 0;

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
    Fonts f; f.body = fonts().body; f.heading = fonts().heading; f.big = fonts().big; f.mono = fonts().mono;
    setFonts(f);
    ImGui::PushFont(fonts().body);
    g_model.readUntil = g_readUntil;
    drawMain(g_model, g_actions);
    g_readUntil = g_model.readUntil;
    ImGui::PopFont();
}

void drawOverlayWin(XpWindow& w) {
    Fonts f; f.body = fonts().body; f.heading = fonts().heading; f.big = fonts().big; f.mono = fonts().mono;
    setFonts(f);
    ImGui::PushFont(fonts().body);
    g_model.readUntil = g_readUntil;
    drawOverlay(g_model, g_actions, w.width());
    ImGui::PopFont();
    w.setPassThrough(!overlayWantsMouse());
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
