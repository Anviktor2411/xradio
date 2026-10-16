// The glue between the plugin and the new window (Phase U): owns the X-Plane windows (main + the cockpit
// overlay, and Tower mode's six), the model the plugin fills every frame, the actions, toasts, theme and scale.
#pragma once

#include <string>

#include "model.h"
#include "tower_ui.h"

namespace xr {
namespace ui2 {
namespace host {

void init(const Actions& actions, const Settings& settings);   // creates the windows (hidden)
void shutdown();
void update(const Model& model);                                // every frame, from the flight loop
void toggleMain();
void showMain(bool on);
bool mainVisible();
void showSettings();
void toast(const std::string& text, int colour);                // 0 info, 1 amber, 2 green, 3 red
void applyDisplay(const Settings& s);                           // theme, scale, overlay, language
void resetPositions();
std::string mainGeometry();                                     // "l,t,r,b"
std::string overlayPosition();                                  // "l,t"
int  currentTab();

// Tower mode's windows (ground radar, strips, Delivery, frequency, ATIS / operations, the connection banner):
// created hidden, shown and hidden every frame from the tower's state. The five work windows open once the
// link and the airport are there; until then only the banner says what is missing.
void initTower(const TowerActions& actions, const std::string& layout);
void updateTower(const tower::State& s, bool active, bool windowsOn, const std::string& code, const std::string& position);
std::string towerLayout();                                      // "l,t,r,b|..." in window order, for the settings file
void resetTowerPositions();

}  // namespace host

void openUrlNative(const std::string& url);   // the system browser
}  // namespace ui2
}  // namespace xr
