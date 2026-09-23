// The XRadio mark, for the top of both windows.
//
// It is drawn with XPLMDrawString like everything else in this UI (see ui.h
// for why there is no OpenGL here): three amber arcs for the signal, the
// name, and the version. The arcs and the name are separate draws so they
// can be different colours, which means their x positions have to be worked
// out from the font's character width rather than guessed.
#pragma once

#include <cstring>

#include "XPLMGraphics.h"

namespace xr {
namespace brand {

inline const char* version() { return "0.5.3"; }

// Width of one character of the fixed-pitch font, asked of X-Plane once.
inline int charWidth() {
    static int w = 0;
    if (w == 0) {
        int h = 0;
        XPLMGetFontDimensions(xplmFont_Basic, &w, &h, nullptr);
        if (w <= 0) w = 7;                 // a sane default if the SDK says nothing
    }
    return w;
}

// Draws `)))  XRadio  v0.5.3` at (x, y). Returns the height it used, so the
// caller can move its cursor down by exactly that much.
inline int draw(int x, int y, bool withTagline = false) {
    float amber[] = {1.f, 0.72f, 0.13f};
    float white[] = {0.95f, 0.97f, 1.f};
    float dim[]   = {0.78f, 0.82f, 0.88f};

    const int cw = charWidth();

    const char* wave = ")))";
    XPLMDrawString(amber, x, y, (char*)wave, nullptr, xplmFont_Basic);

    int cx = x + (int)strlen(wave) * cw + cw;
    const char* name = "XRadio";
    XPLMDrawString(white, cx, y, (char*)name, nullptr, xplmFont_Basic);

    cx += (int)strlen(name) * cw + cw;
    char ver[32];
    snprintf(ver, sizeof(ver), "v%s", version());
    XPLMDrawString(dim, cx, y, ver, nullptr, xplmFont_Basic);

    if (withTagline) {
        cx += (int)strlen(ver) * cw + cw * 2;
        XPLMDrawString(dim, cx, y, (char*)"multiplayer + VHF radio",
                       nullptr, xplmFont_Basic);
    }
    return 18;
}

}  // namespace brand
}  // namespace xr
