// The glue between the plugin and the new window (Phase U): owns the X-Plane windows (main + the cockpit
// overlay), the model the plugin fills every frame, the actions, toasts, theme and scale.
#pragma once

#include <string>

#include "model.h"

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

}  // namespace host

void openUrlNative(const std::string& url);   // the system browser
}  // namespace ui2
}  // namespace xr
