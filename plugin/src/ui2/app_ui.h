// The window itself (Phase U): the sidebar and its tabs, the compact cockpit overlay, toasts and the
// first-run wizard -- all plain Dear ImGui over the Model, nothing from X-Plane. The host (host.cpp) owns
// the X-Plane windows and calls drawMain()/drawOverlay() inside them; tools/ui_preview renders the same
// functions to PNG files for the documentation.
#pragma once

#include <string>

#include "model.h"

namespace xr {
namespace ui2 {

struct Fonts { void* body = nullptr; void* heading = nullptr; void* big = nullptr; void* mono = nullptr; void* small = nullptr; };   // ImFont*

struct UiState {
    int         tab = 0;                    // 0 Radio, 1 ATC, 2 Pilots, 3 Messages, 4 Flight plan, 5 Host, 6 Settings, 7 Tower
    char        chatInput[320] = {0};
    bool        chatFocus = false;
    char        freqInput[2][16] = {{0}, {0}};
    char        pilotFilter[32] = {0};
    bool        hideAi = false;
    Settings    edit;                       // the settings being edited
    bool        editLoaded = false, editDirty = false;
    int         settingsGroup = 0;          // 0 audio, 1 controls, 2 display, 3 traffic, 4 language, 5 tower
    bool        capturingPtt = false, capturingTower = false;
    bool        wizardOpen = false;
    Wizard      wizard;
    std::string note;                       // a short confirmation ("copied", "filed")
    double      noteAt = -100;
    Toasts      toasts;
    double      lastActivity = 0;           // for the overlay's auto-hide
    bool        overlayHover = false;
    bool        micTest = false;
    FlightPlanView fpEdit;                  // the flight plan form
    bool        fpLoaded = false;
    int         fpSerial = -1;              // the OFP import the form was loaded from
    Fonts       fonts;
    bool        wantSettingsTab = false;
};

UiState& state();
void   setFonts(const Fonts& f);
void   toast(const std::string& text, int colour, double now);
void   drawMain(Model& m, const Actions& a);                  // inside an ImGui window the host began
void   drawOverlay(Model& m, const Actions& a, float width);  // the compact strip + toasts
bool   overlayWantsMouse();                                   // false: clicks fall through to the sim
void   noteKey(int vk, bool down);                            // a raw key for the PTT / tower bindings capture
const char* tabName(int tab);
int    tabCount(bool withTower);

}  // namespace ui2
}  // namespace xr
