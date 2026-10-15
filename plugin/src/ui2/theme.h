// Colours with a meaning (green = OK / receiving, red = transmitting / error, amber = warning), a dark theme
// that fits a cockpit at night and a light one, consistent spacing. Pure ImGui style; no X-Plane.
#pragma once
#include "imgui.h"

namespace xr {
namespace ui2 {

struct Palette {
    ImVec4 bg, panel, panelAlt, text, muted, line, accent, ok, warn, err, rx, tx, atc, priv, ai;
};

const Palette& palette();
void applyTheme(const char* name, float scale);   // "dark" / "light"; scale 0.75..2 (fonts are scaled separately)
ImU32 col(const ImVec4& v, float alpha = 1.f);

// Small drawn icons (no icon font needed): a round light, signal bars, a level meter.
void light(const ImVec4& colour, bool on, float r = 6.f);
void signalBarsWidget(int bars, int max, const ImVec4& colour);
void meter(float level01, const ImVec4& colour, float w = 120.f, float h = 8.f);
void badge(const char* text, const ImVec4& colour);

}  // namespace ui2
}  // namespace xr
