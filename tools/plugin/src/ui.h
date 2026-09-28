// A very small immediate-mode widget set drawn entirely with XPLMDrawString.
//
// Why text only: X-Plane 12 runs Vulkan/Metal, where raw OpenGL in a window
// callback is discouraged, and XPLMDrawString is the one drawing primitive
// that is guaranteed everywhere. Sliders are `[####------]`, toggles are
// `[x]`, dropdowns are `< name >`. It also means the whole UI is a list of
// strings, which the test harness can capture and assert on.
//
// Usage per frame: fill in Ctx (geometry, plus any click or key that arrived
// since the last frame), call the widgets in order, then clear the input.
#pragma once

#include <string>
#include <vector>

namespace xr {
namespace ui {

struct Ctx {
    // geometry, set from XPLMGetWindowGeometry
    int left = 0, top = 0, right = 0, bottom = 0;

    // pending input, consumed by whichever widget owns that row
    bool clicked = false;
    int  clickX = 0, clickY = 0;

    // Keyboard goes to the focused text field. Keys are queued rather than
    // held one at a time: X-Plane delivers them as they arrive, which can be
    // several per frame, and a single slot silently loses the rest.
    struct KeyEvent {
        char ch = 0;          // printable character, 0 for none
        int  vk = 0;          // XPLM_VK_*, 0 for none
    };
    int  focus = -1;                 // index among focusable widgets, -1 for none
    std::vector<KeyEvent> keys;      // since the last frame, in arrival order
    static constexpr size_t kMaxKeys = 64;   // a window that never draws

    void pushKey(char ch, int vk) {
        if (keys.size() < kMaxKeys) keys.push_back({ch, vk});
    }

    // running state
    int  y = 0;               // baseline of the row about to be drawn
    int  index = 0;           // focusable-widget counter
    bool blink = true;        // cursor phase

    static constexpr int kRowH   = 22;
    static constexpr int kValueX = 190;   // where values start, from `left`

    void begin(int l, int t, int r, int b, int firstRowOffset) {
        left = l; top = t; right = r; bottom = b;
        y = t - firstRowOffset;
        index = 0;
    }
    void endInput() { clicked = false; keys.clear(); }

    bool rowHit() const {
        return clicked && clickY >= y - 6 && clickY <= y + 15;
    }
    void nextRow() { y -= kRowH; }
};

// Row of tabs; returns true if the selection changed.
bool tabs(Ctx& c, const char* const* names, int count, int& current);

// Editable text. Returns true if the value changed.
bool textField(Ctx& c, const char* label, std::string& value,
               size_t maxLen, bool digitsOnly);

// `[x] Label`. Returns true if toggled.
bool toggle(Ctx& c, const char* label, bool& value);

// `Label  [#####-----]  42 unit`. Click anywhere on the bar to set.
bool slider(Ctx& c, const char* label, float& value, float lo, float hi,
            const char* unit, bool percent);

// `Label  < current >`, arrows cycle. `empty` is shown when options is empty.
bool choice(Ctx& c, const char* label, std::string& value,
            const std::vector<std::string>& options, const char* empty);

// Plain text line, no interaction.
void text(Ctx& c, const char* s, int colour = 0);   // 0 white, 1 grey, 2 green, 3 amber

// A row of buttons; returns the index clicked, or -1.
int buttons(Ctx& c, const char* const* labels, int count);

// How many focusable widgets the last pass drew (for Tab cycling).
int focusableCount(const Ctx& c);

}  // namespace ui
}  // namespace xr
