// Window placement tests.
//
// Reproduces the monitor layouts that put XRadio's window off-screen -- a
// second monitor to the LEFT of the main one (so the global desktop starts at
// a negative x, which is what Linux multi-monitor users hit), a tall portrait
// setup, a tiny screen, and a garbage report -- and asserts the window always
// lands fully inside a usable area.
#include "harness.h"

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
int  XPluginStart(char*, char*, char*);
void XPluginStop(void);
}

static int failures = 0;

static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

// The area the window must end up inside: the monitor if one was reported,
// otherwise the global screen bounds.
struct Area { int l, t, r, b; };

static void scenario(const std::string& name, Area screen,
                     const std::vector<Area>& monitors, Area expectIn) {
    printf("\n%s\n", name.c_str());

    harness::resetWindows();
    harness::setScreen(screen.l, screen.t, screen.r, screen.b);
    harness::clearMonitors();
    for (const auto& m : monitors) harness::addMonitor(m.l, m.t, m.r, m.b);

    char n[256], s[256], d[256];
    XPluginStart(n, s, d);

    for (int id = 1; id <= 2; ++id) {
        int l, t, r, b;
        harness::windowRect(id, &l, &t, &r, &b);
        const std::string who = (id == 1 ? "main window" : "settings window");
        char pos[128];
        snprintf(pos, sizeof(pos), "l=%d t=%d r=%d b=%d", l, t, r, b);

        check(who + " has a sane size", (r - l) >= 300 && (t - b) >= 200, pos);
        check(who + " left edge on screen",   l >= expectIn.l, pos);
        check(who + " right edge on screen",  r <= expectIn.r, pos);
        check(who + " top edge on screen",    t <= expectIn.t, pos);
        check(who + " bottom edge on screen", b >= expectIn.b, pos);
    }

    XPluginStop();
}

int main() {
    // The reported failure: a second monitor left of the main one, so the
    // global desktop spans x = -1920 .. 1920 and the old code placed the
    // window at globalLeft+60, i.e. far off to the left.
    scenario("two monitors, second one to the LEFT (the Linux report)",
             {-1920, 1080, 1920, 0},
             {{0, 1080, 1920, 0}},
             {0, 1080, 1920, 0});

    // Windowed mode reports no full-screen monitors at all.
    scenario("windowed mode, no monitors reported",
             {0, 1080, 1920, 0}, {},
             {0, 1080, 1920, 0});

    // Monitors stacked vertically: global top is far above the usable one.
    scenario("two monitors stacked vertically",
             {0, 2160, 1920, 0},
             {{0, 1080, 1920, 0}},
             {0, 1080, 1920, 0});

    // A small laptop screen: windows must shrink rather than overflow.
    scenario("small 1280x720 screen",
             {0, 720, 1280, 0},
             {{0, 720, 1280, 0}},
             {0, 720, 1280, 0});

    // Nonsense from the sim: fall back to something usable, never off-screen.
    scenario("degenerate bounds reported",
             {0, 0, 0, 0}, {},
             {0, 800, 1280, 0});

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all placement tests passed\n");
    return 0;
}
