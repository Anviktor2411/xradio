// Tower mode's windows in Dear ImGui, with the main window's theme and fonts: the ground radar, the strip bays,
// Delivery, the frequency (log and command line), ATIS / operations, and one connection banner. They draw
// tower::State and act through TowerActions -- the logic stays in tower.cpp. Plain ImGui, nothing from
// X-Plane: the host (host.cpp) owns the X-Plane windows, tools/ui_preview renders these functions to PNG and
// tools/harness/tower_ui_test clicks through them.
#pragma once

#include <functional>
#include <map>
#include <string>

#include "../tower.h"
#include "app_ui.h"
#include "tower_model.h"

namespace xr {
namespace ui2 {

struct TowerActions {
    std::function<void(int id)> select;                          // -1: nothing
    std::function<void(const std::string& text)> transmit;       // a typed instruction, as my transmission
    std::function<void(int id)> accept;                          // an incoming handoff
    std::function<void(int id, const std::string& bay)> moveStrip;
    std::function<void(bool on)> setLvp;
    std::function<void(const std::string& end)> setRunway;
    std::function<void(bool on)> setStopBars;
    std::function<void(int id, const std::string& signal)> lightGun;
    std::function<void()> retry;                                 // the link, now
    std::function<void()> leave;                                 // out of Tower mode
    std::function<void()> settings;                              // XRadio's Settings > Tower
    std::function<void(float dYawDeg, float dPitchDeg)> look;    // the camera in the cab
    std::function<void(float factor)> binoculars;
    std::function<void(int id)> follow;                          // the camera follows; -1 lets go
    std::function<void(int id)> lookAt;
    std::function<void(int which, bool out)> popOut;             // a window into its own OS window, and back
};

enum TowerWindow { kTwRadar = 0, kTwStrips, kTwDelivery, kTwFrequency, kTwOps, kTwBanner, kTwCount };
const char* towerWindowTitle(int which);

// What a window needs to know about itself and the settings, besides State.
struct TowerFrame {
    int         which = kTwRadar;
    bool        poppedOut = false, inVR = false;
    std::string code = "TOWER", position = "TWR";   // the session code and the position from the settings
};

// What the tower windows share between them (each has its own ImGui context): the command line, the radar.
struct TowerUi {
    tw::View    view;
    std::string viewIcao;                 // the airport the view was fitted to
    bool        viewFitted = false;
    char        cmd[320] = {0};
    bool        cmdReplace = false;       // another window changed cmd: the text field takes it over
    bool        focusCmd = false;         // the keyboard goes to the command line on its next frame
    bool        wantFrequencyFront = false;   // the host brings the frequency window forward
    tw::History history;
    bool        labels = true, stands = true;
    int         contextId = -1;           // the aircraft a radar right-click menu is about
    int         hoverId = -1;             // the aircraft under the mouse on the radar: its whole label
    std::map<int, int> labelSlot;         // where each label went last frame -- it stays there while that is free
};
TowerUi& towerUi();
void setTowerFonts(const Fonts& f);       // body / heading / big / small, as the host built them

// One window's content, inside the ImGui window the host began (or the preview, or the test).
void drawTower(const TowerFrame& f, const tower::State& s, const TowerActions& a);

// Test hooks: the screen rectangle of a named control from the last frame ("strip:FIN321", "clear:BTI4KT", "cmd",
// "lvp", "light:green steady", "retry", "radar", "ac:DLH5KM" ...). Off unless a test turns them on.
struct ProbeRect { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool found = false; };
void enableProbes(bool on);
ProbeRect probe(const std::string& name);
void clearProbes();

}  // namespace ui2
}  // namespace xr
