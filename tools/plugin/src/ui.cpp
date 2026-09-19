#include "ui.h"

#include "XPLMDefs.h"
#include "XPLMGraphics.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace xr {
namespace ui {

namespace {

float kWhite[] = {1.f, 1.f, 1.f};
float kGrey[]  = {0.68f, 0.68f, 0.68f};
float kGreen[] = {0.40f, 1.f, 0.40f};
float kAmber[] = {1.f, 0.80f, 0.30f};

float* colour(int c) {
    switch (c) {
        case 1:  return kGrey;
        case 2:  return kGreen;
        case 3:  return kAmber;
        default: return kWhite;
    }
}

void draw(const Ctx& c, int x, const char* s, int col) {
    XPLMDrawString(colour(col), c.left + x, c.y, (char*)s, nullptr, xplmFont_Proportional);
}

constexpr int kBarCells = 16;      // characters in a slider track
constexpr int kCellPx   = 7;       // approximate width of one character
constexpr int kBarX     = Ctx::kValueX;

}  // namespace

bool tabs(Ctx& c, const char* const* names, int count, int& current) {
    bool changed = false;
    int x = 10;
    for (int i = 0; i < count; ++i) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s %s",
                 i == current ? "[" : " ", names[i], i == current ? "]" : " ");
        const int w = (int)(strlen(buf) + 1) * kCellPx;
        if (c.clicked && c.clickY >= c.y - 6 && c.clickY <= c.y + 15 &&
            c.clickX >= c.left + x && c.clickX < c.left + x + w) {
            if (current != i) { current = i; changed = true; }
        }
        draw(c, x, buf, i == current ? 2 : 1);
        x += w;
    }
    c.nextRow();
    return changed;
}

bool textField(Ctx& c, const char* label, std::string& value,
               size_t maxLen, bool digitsOnly) {
    const int self = c.index++;
    const bool focused = (c.focus == self);
    bool changed = false;

    if (c.rowHit()) {
        c.focus = self;
        c.clicked = false;
    }

    if (focused && !c.keys.empty()) {
        for (const auto& ev : c.keys) {
            const unsigned char ch = (unsigned char)ev.ch;
            if (ev.vk == 0x08 || ch == 8) {               // backspace
                if (!value.empty()) { value.pop_back(); changed = true; }
            } else if (ch >= 32 && ch < 127 && value.size() < maxLen) {
                if (!digitsOnly || isdigit(ch)) {
                    value += (char)ch;
                    changed = true;
                }
            }
        }
        c.keys.clear();
    }

    draw(c, 10, label, focused ? 0 : 1);
    std::string shown = value;
    if (focused && c.blink) shown += "_";
    char buf[128];
    snprintf(buf, sizeof(buf), "%s%s", focused ? "> " : "  ", shown.c_str());
    draw(c, kBarX, buf, focused ? 2 : 0);
    c.nextRow();
    return changed;
}

bool toggle(Ctx& c, const char* label, bool& value) {
    const int self = c.index++;
    bool changed = false;
    if (c.rowHit()) {
        value = !value;
        changed = true;
        c.focus = self;
        c.clicked = false;
    }
    draw(c, 10, label, 1);
    draw(c, kBarX, value ? "[x]  on" : "[ ]  off", value ? 2 : 1);
    c.nextRow();
    return changed;
}

bool slider(Ctx& c, const char* label, float& value, float lo, float hi,
            const char* unit, bool percent) {
    const int self = c.index++;
    bool changed = false;

    const int barLeft  = c.left + kBarX + kCellPx;        // just inside the '['
    const int barRight = barLeft + kBarCells * kCellPx;
    if (c.rowHit() && c.clickX >= barLeft - kCellPx && c.clickX <= barRight + kCellPx) {
        float f = (float)(c.clickX - barLeft) / (float)(barRight - barLeft);
        f = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
        const float nv = lo + f * (hi - lo);
        if (nv != value) { value = nv; changed = true; }
        c.focus = self;
        c.clicked = false;
    }

    const float span = (hi - lo) > 1e-6f ? (hi - lo) : 1.f;
    float frac = (value - lo) / span;
    frac = frac < 0.f ? 0.f : (frac > 1.f ? 1.f : frac);
    const int filled = (int)(frac * kBarCells + 0.5f);

    char bar[kBarCells + 3];
    bar[0] = '[';
    for (int i = 0; i < kBarCells; ++i) bar[i + 1] = i < filled ? '#' : '-';
    bar[kBarCells + 1] = ']';
    bar[kBarCells + 2] = '\0';

    char buf[96];
    if (percent) {
        snprintf(buf, sizeof(buf), "%s  %3d%%", bar, (int)(frac * 100.f + 0.5f));
    } else {
        snprintf(buf, sizeof(buf), "%s  %g%s", bar, (double)value, unit);
    }
    draw(c, 10, label, 1);
    draw(c, kBarX, buf, 0);
    c.nextRow();
    return changed;
}

bool choice(Ctx& c, const char* label, std::string& value,
            const std::vector<std::string>& options, const char* empty) {
    const int self = c.index++;
    bool changed = false;

    int cur = -1;
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i] == value) { cur = (int)i; break; }
    }

    const int arrowLeftX  = c.left + kBarX;
    const int arrowRightX = c.left + kBarX + 2 * kCellPx;
    if (c.rowHit() && !options.empty()) {
        const int n = (int)options.size();
        if (c.clickX >= arrowLeftX && c.clickX < arrowRightX) {
            cur = (cur <= 0) ? n - 1 : cur - 1;
            value = options[(size_t)cur];
            changed = true;
        } else if (c.clickX >= arrowRightX) {
            cur = (cur < 0 || cur >= n - 1) ? 0 : cur + 1;
            value = options[(size_t)cur];
            changed = true;
        }
        c.focus = self;
        c.clicked = false;
    }

    char buf[160];
    const char* shown = value.empty() ? empty : value.c_str();
    snprintf(buf, sizeof(buf), "< > %s", shown);
    draw(c, 10, label, 1);
    draw(c, kBarX, buf, value.empty() ? 1 : 0);
    c.nextRow();
    return changed;
}

void text(Ctx& c, const char* s, int col) {
    draw(c, 10, s, col);
    c.nextRow();
}

int buttons(Ctx& c, const char* const* labels, int count) {
    int hit = -1;
    int x = 10;
    for (int i = 0; i < count; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "[ %s ]", labels[i]);
        const int w = (int)(strlen(buf) + 2) * kCellPx;
        if (c.clicked && c.clickY >= c.y - 6 && c.clickY <= c.y + 15 &&
            c.clickX >= c.left + x && c.clickX < c.left + x + w) {
            hit = i;
            c.clicked = false;
        }
        draw(c, x, buf, i == 0 ? 2 : 1);
        x += w;
    }
    c.nextRow();
    return hit;
}

int focusableCount(const Ctx& c) { return c.index; }

}  // namespace ui
}  // namespace xr
