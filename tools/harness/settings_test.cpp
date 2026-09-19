// Settings: the model, the config file, and every widget in the window.
//
// The window is drawn entirely with XPLMDrawString, so the harness can read
// back exactly what a pilot would see and click exactly where they would
// click -- the clicks below use the coordinates the draw actually reported,
// not a re-derived copy of the layout that could drift out of sync with it.
#include "harness.h"
#include "settings.h"
#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
int  XPluginStart(char*, char*, char*);
int  XPluginEnable(void);
void XPluginDisable(void);
void XPluginStop(void);
}

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

static const char* kCfgPath = "/tmp/xradio-harness/xradio.cfg";

// The settings window is the second window the plugin creates.
static constexpr int kWin = 2;

static std::vector<std::string> draw() { return harness::drawWindow(kWin); }

static bool shows(const std::vector<std::string>& lines, const std::string& needle) {
    return harness::drawnContains(lines, needle);
}

static void dump(const std::vector<std::string>& lines) {
    for (const auto& l : lines) printf("        %s\n", l.c_str());
}

// Click the row a label was drawn on, `xOffset` pixels from the window's left
// edge. Redraws first so the coordinates belong to what is on screen now.
static bool clickRow(const std::string& label, int xOffset) {
    draw();
    int lx = 0, ly = 0;
    if (!harness::drawnAt(label, &lx, &ly)) return false;
    int top = 0, left = 0;
    harness::windowTop(kWin, &top, &left);
    harness::click(kWin, left + xOffset, ly);
    draw();                    // the click is consumed by the next draw
    return true;
}

// The value half of a row, in window-local pixels.
static constexpr int kValueX = xr::ui::Ctx::kValueX;

int main() {
    if (system("mkdir -p /tmp/xradio-harness") != 0) return 1;
    remove(kCfgPath);

    printf("\nsettings model\n");
    {
        xr::Settings s;
        check("sane defaults", s.port_i() == 49100 && s.volume > 0.f && s.showLabels);
        check("every field has a unique config key", [] {
            xr::Settings t;
            auto f = xr::describe(t);
            std::vector<std::string> keys;
            for (auto& x : f) keys.push_back(x.key);
            std::sort(keys.begin(), keys.end());
            return std::unique(keys.begin(), keys.end()) == keys.end();
        }());
        check("every field points at a distinct member", [] {
            xr::Settings t;
            auto f = xr::describe(t);
            std::vector<void*> ptrs;
            for (auto& x : f) ptrs.push_back(x.ptr);
            std::sort(ptrs.begin(), ptrs.end());
            return std::unique(ptrs.begin(), ptrs.end()) == ptrs.end();
        }());
        check("every field lands on a real tab", [] {
            xr::Settings t;
            for (auto& x : xr::describe(t)) {
                if (x.tab < 0 || x.tab >= xr::kNumTabs) return false;
            }
            return true;
        }());
        check("every tab has at least one field", [] {
            xr::Settings t;
            std::vector<int> n((size_t)xr::kNumTabs, 0);
            for (auto& x : xr::describe(t)) ++n[x.tab];
            for (int c : n) if (c == 0) return false;
            return true;
        }());
        check("sliders have a usable range", [] {
            xr::Settings t;
            for (auto& x : xr::describe(t)) {
                if (x.kind == xr::Kind::Slider && !(x.hi > x.lo)) return false;
            }
            return true;
        }());
        check("defaults are inside their slider ranges", [] {
            xr::Settings t;
            for (auto& x : xr::describe(t)) {
                if (x.kind != xr::Kind::Slider) continue;
                const float v = *(float*)x.ptr;
                if (v < x.lo || v > x.hi) return false;
            }
            return true;
        }());
    }

    printf("\nconfig file round trip\n");
    {
        xr::Settings a;
        a.host = "fly.example.net";
        a.port = "49200";
        a.callsign = "SU-CBB";
        a.acIcao = "A20N";
        a.autoConnect = false;
        a.reportHz = 8.f;
        a.smoothMs = 500.f;
        a.micDevice = "Headset Microphone (USB)";
        a.outDevice = "Speakers (Realtek)";
        a.volume = 0.42f;
        a.sidetone = true;
        a.hiss = 0.f;
        a.radioFilter = false;
        a.showTraffic = false;
        a.showLabels = false;
        a.labelDistNm = 45.f;
        a.trafficRangeNm = 120.f;
        check("save writes the file", xr::saveSettings(a, kCfgPath));

        xr::Settings b;
        check("load reads it back", xr::loadSettings(b, kCfgPath));
        check("strings survive", b.host == a.host && b.callsign == a.callsign &&
                                 b.acIcao == a.acIcao);
        check("device names with spaces and brackets survive",
              b.micDevice == a.micDevice && b.outDevice == a.outDevice,
              "'" + b.micDevice + "'");
        check("booleans survive", b.autoConnect == false && b.sidetone == true &&
                                  b.radioFilter == false && b.showTraffic == false);
        check("numbers survive", b.reportHz == 8.f && b.smoothMs == 500.f &&
                                 b.labelDistNm == 45.f && b.trafficRangeNm == 120.f);
        check("volume survives", b.volume > 0.41f && b.volume < 0.43f,
              std::to_string(b.volume));

        // A setting that is saved but never read back -- or read back but
        // never saved -- is the classic descriptor-table bug, and it stays
        // invisible unless something walks every field.
        check("nothing is saved-but-not-loaded", [&] {
            xr::Settings d;
            if (!xr::loadSettings(d, kCfgPath)) return false;
            auto fa = xr::describe(b);
            auto fb = xr::describe(d);
            if (fa.size() != fb.size()) return false;
            for (size_t i = 0; i < fa.size(); ++i) {
                switch (fa[i].kind) {
                    case xr::Kind::Text:
                    case xr::Kind::Choice:
                        if (*(std::string*)fa[i].ptr != *(std::string*)fb[i].ptr) return false;
                        break;
                    case xr::Kind::Bool:
                        if (*(bool*)fa[i].ptr != *(bool*)fb[i].ptr) return false;
                        break;
                    case xr::Kind::Slider: {
                        const float x = *(float*)fa[i].ptr, y = *(float*)fb[i].ptr;
                        if (x - y > 0.001f || y - x > 0.001f) return false;
                        break;
                    }
                }
            }
            return true;
        }());
    }

    printf("\nhand-edited nonsense is clamped or rejected\n");
    {
        FILE* f = fopen(kCfgPath, "w");
        fprintf(f, "host = fly.example.net\nport = 49200\ncallsign = X\n"
                   "volume = 99\nhiss = -5\nreporthz = 1000\nsmoothms = 1\n"
                   "labeldist = 100000\nnot_a_key = 7\ngarbage line without equals\n");
        fclose(f);
        xr::Settings s;
        xr::loadSettings(s, kCfgPath);
        check("out-of-range sliders clamp to their limits",
              s.volume == 1.f && s.hiss == 0.f && s.reportHz == 10.f &&
              s.smoothMs == 100.f && s.labelDistNm == 100.f,
              std::to_string(s.volume) + "/" + std::to_string(s.reportHz));
        check("unknown keys and junk lines are ignored", s.host == "fly.example.net");

        s.host = "  ";
        check("empty host is rejected", xr::validate(s) == "Host cannot be empty");
        s.host = "x"; s.port = "0";
        check("port 0 is rejected", xr::validate(s) == "Port must be 1-65535");
        s.port = "70000";
        check("port above 65535 is rejected", xr::validate(s) == "Port must be 1-65535");
        s.port = "49200"; s.callsign = "";
        check("empty callsign is rejected", xr::validate(s) == "Callsign cannot be empty");
        s.callsign = "OK";
        check("a valid set passes", xr::validate(s).empty());
        check("a missing file is not a crash", !xr::loadSettings(s, "/tmp/xradio-harness/nope.cfg"));
    }

    printf("\nthe window itself\n");
    {
        // Start the plugin with a known config so the window has something in it.
        xr::Settings s;
        s.host = "fly.example.net";
        s.port = "49200";
        s.callsign = "SU-CBB";
        s.autoConnect = false;          // no network in this test
        s.volume = 0.5f;
        s.sidetone = false;
        s.showLabels = true;
        xr::saveSettings(s, kCfgPath);

        harness::resetWindows();
        char n[256], sg[256], d[256];
        XPluginStart(n, sg, d);
        XPluginEnable();

        check("settings window starts hidden", !harness::windowVisible(kWin));
        harness::menu(1);                                  // Plugins > Settings...
        check("menu opens it", harness::windowVisible(kWin));

        auto lines = draw();
        check("tabs are drawn", shows(lines, "Connection") && shows(lines, "Audio") &&
                                shows(lines, "Traffic"));
        check("connection tab shows its fields",
              shows(lines, "Server host") && shows(lines, "Port") &&
              shows(lines, "Callsign") && shows(lines, "Report rate"));
        check("current values are shown", shows(lines, "fly.example.net") &&
                                          shows(lines, "SU-CBB"));
        check("a slider renders as a bar", shows(lines, "[#") || shows(lines, "[-"));
        check("a toggle renders", shows(lines, "[x]") || shows(lines, "[ ]"));
        check("buttons are drawn", shows(lines, "Save & apply") && shows(lines, "Cancel"));
        check("audio fields are NOT on the connection tab",
              !shows(lines, "Microphone") && !shows(lines, "Carrier hiss"));
        if (failures) dump(lines);

        printf("\ntabs\n");
        {
            int top = 0, left = 0, tx = 0, ty = 0;
            harness::windowTop(kWin, &top, &left);
            check("the Audio tab was drawn", harness::drawnAt("Audio", &tx, &ty));
            harness::click(kWin, tx + 10, ty);
            auto audio = draw();
            check("clicking it switches tab",
                  shows(audio, "Microphone") && shows(audio, "Carrier hiss") &&
                  shows(audio, "Volume"));
            check("connection fields are gone", !shows(audio, "Server host"));
            check("the mic meter is on the audio tab", shows(audio, "Mic level"));
            if (failures) dump(audio);

            check("the Traffic tab was drawn", harness::drawnAt("Traffic", &tx, &ty));
            harness::click(kWin, tx + 10, ty);
            auto traffic = draw();
            check("traffic tab shows its fields",
                  shows(traffic, "Draw other aircraft") && shows(traffic, "Label range") &&
                  shows(traffic, "Traffic range"));
            check("audio fields are gone", !shows(traffic, "Carrier hiss"));
            if (failures) dump(traffic);
        }

        printf("\nwidgets respond to clicks\n");
        {
            // Back to Audio, where there is one of each kind.
            int tx = 0, ty = 0;
            harness::drawnAt("Audio", &tx, &ty);
            harness::click(kWin, tx + 10, ty);
            auto audio = draw();

            check("volume starts at 50%", shows(audio, " 50%"),
                  "expected the saved 0.5");

            // The slider track starts one cell right of the value column and
            // is 16 cells wide; clicking past its end means full scale.
            int top = 0, left = 0;
            harness::windowTop(kWin, &top, &left);
            check("clicked the volume row", clickRow("Volume", kValueX + 16 * 7 + 7));
            check("clicking the end of the bar sets 100%",
                  shows(draw(), "100%"));
            check("clicking the start of the bar sets 0%",
                  clickRow("Volume", kValueX + 7) && shows(draw(), "  0%"));

            check("toggle starts off", shows(draw(), "[ ]"));
            check("clicked the sidetone row", clickRow("Hear own voice", kValueX + 10));
            {
                auto after = draw();
                // Find the sidetone row specifically rather than any toggle.
                bool on = false;
                for (size_t i = 0; i < after.size(); ++i) {
                    if (after[i].find("Hear own voice") == std::string::npos) continue;
                    if (i + 1 < after.size() && after[i + 1].find("[x]") != std::string::npos) {
                        on = true;
                    }
                    break;
                }
                check("clicking a toggle flips it", on);
                if (!on) dump(after);
            }

            // A Choice with no devices behind it (null audio backend) must not
            // crash or invent a name when its arrows are clicked.
            check("clicked the microphone row", clickRow("Microphone", kValueX + 2));
            check("an empty device list stays empty",
                  shows(draw(), "system default"));
        }

        printf("\ntyping and saving\n");
        {
            // Connection tab, edit the callsign, save, and check the file.
            int tx = 0, ty = 0;
            harness::drawnAt("Connection", &tx, &ty);
            harness::click(kWin, tx + 10, ty);
            draw();

            check("clicked the callsign field", clickRow("Callsign", kValueX + 10));
            // Several keys can land between two frames; none may be lost.
            harness::typeText(kWin, "99");
            auto typed = draw();
            check("a burst of keys all lands in the focused field",
                  shows(typed, "SU-CBB99"));
            if (!shows(typed, "SU-CBB99")) dump(typed);

            harness::pressVk(kWin, 0x08);                  // backspace
            harness::pressVk(kWin, 0x08);
            check("backspace deletes", shows(draw(), "SU-CBB") &&
                                       !shows(draw(), "SU-CBB9"));

            // Port is digits only: letters must not get in.
            check("clicked the port field", clickRow("Port", kValueX + 10));
            harness::typeText(kWin, "abc");
            check("letters are refused in a digits-only field",
                  !shows(draw(), "49200a"));
            harness::typeText(kWin, "7");
            check("a full field refuses more digits", shows(draw(), "49200"),
                  "port is capped at 5 characters");

            // Clear it and type a port that is in range for the field but
            // not for TCP.
            for (int i = 0; i < 6; ++i) harness::pressVk(kWin, 0x08);
            harness::typeText(kWin, "70000");
            check("the field shows what was typed", shows(draw(), "70000"));

            int bx = 0, by = 0;
            check("the save button was drawn", harness::drawnAt("Save & apply", &bx, &by));
            harness::click(kWin, bx + 20, by);
            auto refused = draw();
            check("an invalid port is refused with a message",
                  shows(refused, "Port must be 1-65535") && harness::windowVisible(kWin));
            if (failures) dump(refused);

            for (int i = 0; i < 6; ++i) harness::pressVk(kWin, 0x08);
            harness::typeText(kWin, "49200");
            draw();
            harness::drawnAt("Save & apply", &bx, &by);
            harness::click(kWin, bx + 20, by);
            draw();
            check("saving closes the window", !harness::windowVisible(kWin));

            xr::Settings saved;
            check("the file was written", xr::loadSettings(saved, kCfgPath));
            check("edits reached the file",
                  saved.callsign == "SU-CBB" && saved.port == "49200" &&
                  saved.sidetone == true && saved.volume == 0.f,
                  saved.callsign + "/" + saved.port + "/vol " +
                      std::to_string(saved.volume));
        }

        printf("\ncancel discards\n");
        {
            harness::menu(1);
            check("reopened", harness::windowVisible(kWin));
            draw();
            check("clicked the callsign field", clickRow("Callsign", kValueX + 10));
            harness::typeText(kWin, "ZZ");
            check("the edit shows", shows(draw(), "SU-CBBZZ"));

            int bx = 0, by = 0;
            harness::drawnAt("Cancel", &bx, &by);
            harness::click(kWin, bx + 10, by);
            draw();
            check("cancel closes the window", !harness::windowVisible(kWin));

            xr::Settings after;
            xr::loadSettings(after, kCfgPath);
            check("cancel did not write", after.callsign == "SU-CBB", after.callsign);

            harness::menu(1);
            check("reopening starts from the saved values",
                  shows(draw(), "SU-CBB") && !shows(draw(), "SU-CBBZZ"));

            // Escape is the other way out. Like Enter, it acts on the next
            // draw, after the fields have taken whatever was typed ahead of it.
            harness::pressVk(kWin, 27);
            draw();
            check("escape closes it too", !harness::windowVisible(kWin));
        }

        printf("\nkeystrokes keep their order\n");
        {
            // X-Plane hands over keys as they arrive, several per frame on a
            // slow one. Each must reach the field that was focused when it was
            // typed -- and Enter must not act before them.
            harness::menu(1);
            draw();
            check("clicked the callsign field", clickRow("Callsign", kValueX + 10));

            harness::typeText(kWin, "1");
            harness::pressVk(kWin, 0x09);          // XPLM_VK_TAB
            harness::typeText(kWin, "2");
            auto f1 = draw();
            check("text typed before Tab lands in the first field",
                  shows(f1, "SU-CBB1"));
            if (!shows(f1, "SU-CBB1")) dump(f1);
            auto f2 = draw();
            check("text typed after Tab lands in the next field",
                  shows(f2, "C1722"));
            if (!shows(f2, "C1722")) dump(f2);

            // The bug this ordering replaced: Enter acted the moment it
            // arrived, so anything typed just before it was silently dropped.
            harness::typeText(kWin, "9");
            harness::pressVk(kWin, 0x0D);          // XPLM_VK_RETURN
            draw();
            check("Enter saves and closes", !harness::windowVisible(kWin));

            xr::Settings saved;
            xr::loadSettings(saved, kCfgPath);
            check("the keystroke just before Enter was saved too",
                  saved.acIcao == "C17229", saved.acIcao);
        }

        XPluginDisable();
        XPluginStop();
    }

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all settings tests passed\n");
    return 0;
}
