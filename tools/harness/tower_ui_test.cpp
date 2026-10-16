// Tower mode's windows clicked through without X-Plane or OpenGL: the real ui2/tower_ui.cpp in an ImGui context of
// its own per window (as in the sim), the airport and traffic from tools/fixtures/tower_preview.jsonl, mouse and
// keys fed in the way X-Plane hands them over, and every action recorded -- so "Clear" in Delivery, a strip dropped
// on another bay, an aircraft clicked on the radar, the light gun or a typed instruction is checked to ask
// tower.cpp for exactly what it should. Also: every window draws in both themes, tiny and with nothing linked.
//   ./tower_ui_test [fixture]
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "../../plugin/src/tower_parse.h"
#include "../../plugin/src/ui2/theme.h"
#include "../../plugin/src/ui2/tower_ui.h"

using namespace xr;
using namespace xr::ui2;

static int fails = 0;
static void check(bool ok, const std::string& what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) ++fails; }

// ---------------------------------------------------------------- what the windows asked for
struct Rec {
    std::vector<std::string> calls;
    bool has(const std::string& c) const { for (const auto& x : calls) if (x == c) return true; return false; }
    std::string last() const { return calls.empty() ? "" : calls.back(); }
};
static Rec rec;

static TowerActions recordingActions() {
    TowerActions a;
    auto num = [](float f) { char b[32]; snprintf(b, sizeof b, "%.2f", f); return std::string(b); };
    a.select = [](int id) { rec.calls.push_back("select " + std::to_string(id)); };
    a.transmit = [](const std::string& t) { rec.calls.push_back("transmit " + t); };
    a.accept = [](int id) { rec.calls.push_back("accept " + std::to_string(id)); };
    a.moveStrip = [](int id, const std::string& bay) { rec.calls.push_back("move " + std::to_string(id) + " " + bay); };
    a.setLvp = [](bool on) { rec.calls.push_back(std::string("lvp ") + (on ? "on" : "off")); };
    a.setRunway = [](const std::string& e) { rec.calls.push_back("runway " + e); };
    a.setStopBars = [](bool on) { rec.calls.push_back(std::string("stopbars ") + (on ? "on" : "off")); };
    a.lightGun = [](int id, const std::string& sg) { rec.calls.push_back("light " + std::to_string(id) + " " + sg); };
    a.retry = []() { rec.calls.push_back("retry"); };
    a.leave = []() { rec.calls.push_back("leave"); };
    a.settings = []() { rec.calls.push_back("settings"); };
    a.look = [num](float y, float p) { rec.calls.push_back("look " + num(y) + " " + num(p)); };
    a.binoculars = [num](float f) { rec.calls.push_back("binoculars " + num(f)); };
    a.follow = [](int id) { rec.calls.push_back("follow " + std::to_string(id)); };
    a.lookAt = [](int id) { rec.calls.push_back("lookat " + std::to_string(id)); };
    a.popOut = [](int w, bool out) { rec.calls.push_back("popout " + std::to_string(w) + (out ? " out" : " in")); };
    return a;
}

// ---------------------------------------------------------------- one window, its own context
static ImFontAtlas* atlas() {
    static ImFontAtlas* a = nullptr;
    if (!a) { a = new ImFontAtlas(); a->AddFontDefault(); unsigned char* px; int w, h; a->GetTexDataAsRGBA32(&px, &w, &h); a->SetTexID((ImTextureID)1); }
    return a;
}

struct Win {
    ImGuiContext* ctx;
    TowerFrame f;
    float w, h;
    TowerActions a;
    Win(int which, float W, float H, const char* theme = "dark") : w(W), h(H) {
        ctx = ImGui::CreateContext(atlas());
        ImGui::SetCurrentContext(ctx);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(w, h);
        io.DeltaTime = 1.f / 60.f;
        applyTheme(theme, 1.f);
        f.which = which; f.code = "TOWER"; f.position = "TWR";
        a = recordingActions();
    }
    ~Win() { ImGui::DestroyContext(ctx); }
    void frame(const tower::State& s) {
        ImGui::SetCurrentContext(ctx);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(w, h));
        ImGui::Begin("##xr", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        drawTower(f, s, a);
        ImGui::End();
        ImGui::Render();
    }
    void frames(const tower::State& s, int n) { for (int i = 0; i < n; ++i) frame(s); }
    void move(const tower::State& s, float x, float y) { ImGui::SetCurrentContext(ctx); ImGui::GetIO().AddMousePosEvent(x, y); frame(s); }
    void button(const tower::State& s, int b, bool down) { ImGui::SetCurrentContext(ctx); ImGui::GetIO().AddMouseButtonEvent(b, down); frame(s); }
    void click(const tower::State& s, float x, float y, int b = 0) { move(s, x, y); button(s, b, true); button(s, b, false); frames(s, 2); }
    bool clickProbe(const tower::State& s, const std::string& name, int b = 0) {
        const ProbeRect r = probe(name);
        if (!r.found) return false;
        click(s, (r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f, b);
        return true;
    }
    void drag(const tower::State& s, float x0, float y0, float x1, float y1, int b = 0) {
        move(s, x0, y0);
        button(s, b, true);
        for (int i = 1; i <= 8; ++i) move(s, x0 + (x1 - x0) * i / 8.f, y0 + (y1 - y0) * i / 8.f);
        button(s, b, false);
        frames(s, 2);
    }
    void wheel(const tower::State& s, float x, float y, float clicks) { move(s, x, y); ImGui::GetIO().AddMouseWheelEvent(0.f, clicks); frame(s); frame(s); }
    void type(const tower::State& s, const std::string& t) { ImGui::SetCurrentContext(ctx); for (char c : t) { ImGui::GetIO().AddInputCharacter((unsigned)c); frame(s); } }
    void key(const tower::State& s, ImGuiKey k) { ImGui::SetCurrentContext(ctx); ImGui::GetIO().AddKeyEvent(k, true); frame(s); ImGui::GetIO().AddKeyEvent(k, false); frame(s); }
};

static tower::State load(const std::string& path) {
    tower::State s;
    s.myId = "x1";
    std::ifstream in(path);
    std::string line;
    bool a = false, b = false;
    while (std::getline(in, line)) { a |= tower::parseAirportLine(line, s); b |= tower::parseStateLine(line, s, "TWR"); }
    check(a && b, "fixture " + path + " has an airport and a state line");
    s.phase = tower::Phase::Linked; s.linked = true; s.endpoint = "127.0.0.1:8781"; s.now = 100.f;
    return s;
}

static const tower::Aircraft* byCs(const tower::State& s, const std::string& cs) { for (const auto& a : s.ac) if (a.cs == cs) return &a; return nullptr; }

int main(int argc, char** argv) {
    const std::string fixture = argc > 1 ? argv[1] : "tools/fixtures/tower_preview.jsonl";
    tower::State s = load(fixture);
    enableProbes(true);
    TowerUi& u = towerUi();

    // ---- the parser, on the trainer's own line formats
    check(s.icao == "EETN" && s.runways.size() == 1 && s.stands.size() == 11 && s.taxiEdges.size() == 14 && s.pavement.size() == 2 && s.pavement[1].size() == 2,
          "airport: runway, stands, taxi network, pavement (with a hole)");
    check(s.ac.size() == 19 && s.delivery.size() == 3 && s.delivery[1].clearance.find("cleared to Riga via TLL1D") != std::string::npos && s.stopBars && s.haveStopBars,
          "state: traffic, the trainer's Delivery list with its clearance text, the stop bars");
    check(byCs(s, "BUS1") && byCs(s, "BUS1")->quiet && byCs(s, "BUS1")->veh && byCs(s, "CLX74K") && byCs(s, "CLX74K")->dormant && !byCs(s, "EZY12AB")->dormant,
          "a service vehicle is quiet, an aircraft parked for the night dormant");
    check(s.log.size() == 9 && s.log[3].from == "FIN321" && s.log[3].time == "0:04" && s.log[3].cls == "pilot dep" && s.log[3].text.rfind("Tallinn Tower", 0) == 0,
          "log entries taken apart: time, sender, class, text");
    const tower::Aircraft* sas = byCs(s, "SAS123");
    check(sas && sas->handoffIn && sas->handoffFrom == "GND" && byCs(s, "BTI4KT")->waitingS >= 64 && byCs(s, "FIN321")->callingUntilMs > s.utcMs,
          "an incoming handoff, a waiting request, a pilot calling");

    // ---- Delivery: Clear puts the trainer's clearance in the command line and selects the aircraft
    {
        Win w(kTwDelivery, 760, 250);
        w.frames(s, 3);
        rec.calls.clear();
        check(w.clickProbe(s, "clear:BTI4KT"), "Delivery shows a Clear button for BTI4KT");
        check(rec.has("select 4") && std::string(u.cmd) == s.delivery[1].clearance && u.focusCmd && u.wantFrequencyFront,
              "Clear selects BTI4KT and puts the trainer's clearance in the command line, focused (" + std::string(u.cmd) + ")");
        check(!probe("clear:RYR8VX").found, "a cleared request has no Clear button");
        rec.calls.clear();
        check(w.clickProbe(s, "del:ESXYZ") && rec.has("select 7"), "clicking a row selects the aircraft");
    }
    // ---- the frequency: send, history, callsign completion
    {
        Win w(kTwFrequency, 720, 330);
        snprintf(u.cmd, sizeof u.cmd, "%s", s.delivery[1].clearance.c_str());
        u.cmdReplace = true; u.focusCmd = true;
        w.frames(s, 3);
        rec.calls.clear();
        w.key(s, ImGuiKey_Enter);
        check(rec.has("transmit " + s.delivery[1].clearance), "Enter sends the clearance as my transmission");
        check(u.cmd[0] == 0 && u.history.size() == 1, "the command line is empty again and the instruction is in the history");
        w.frames(s, 3);         // ImGui gives the keyboard back to the field a frame after asking; nobody types faster
        check(ImGui::GetIO().WantTextInput, "after sending, the command line keeps the keyboard for the next instruction");
        w.type(s, "FI");
        w.key(s, ImGuiKey_Tab);
        check(std::string(u.cmd) == "FIN321 ", "Tab completes the callsign (FI -> FIN321): " + std::string(u.cmd));
        w.type(s, "line up runway 26");
        w.key(s, ImGuiKey_UpArrow);
        check(std::string(u.cmd) == s.delivery[1].clearance, "Up recalls the last instruction");
        w.key(s, ImGuiKey_DownArrow);
        check(std::string(u.cmd) == "FIN321 line up runway 26", "Down comes back to what was being typed");
        w.key(s, ImGuiKey_Escape);
        check(u.cmd[0] == 0, "Esc clears the line");
        w.type(s, "SA");
        check(probe("chip:SAS123").found, "the callsigns the first word could be are offered as chips");
        rec.calls.clear();
        w.clickProbe(s, "chip:SAS123");
        w.frames(s, 2);
        check(rec.has("select 2") && std::string(u.cmd) == "SAS123 ", "a chip selects the aircraft and fills in its callsign");
        rec.calls.clear();
        snprintf(u.cmd, sizeof u.cmd, "FIN321 runway 26 cleared for takeoff"); u.cmdReplace = true;
        w.frames(s, 2);
        check(w.clickProbe(s, "send") && rec.has("transmit FIN321 runway 26 cleared for takeoff"), "the Send button sends too");
    }
    // ---- strips: select, accept, drag to another bay
    {
        u = TowerUi();
        Win w(kTwStrips, 560, 900);
        w.frames(s, 3);
        rec.calls.clear();
        check(w.clickProbe(s, "strip:FIN321") && rec.has("select 1") && std::string(u.cmd) == "FIN321 ", "clicking a strip selects it and starts the instruction with its callsign");
        rec.calls.clear();
        check(probe("bay:incoming").found && w.clickProbe(s, "accept:SAS123") && rec.has("accept 2") && !rec.has("select 2"), "Accept on the incoming strip accepts the handoff (and only that)");
        const ProbeRect card = probe("strip:LOT789"), bay = probe("bay:holding");
        check(card.found && bay.found, "LOT789 is in the Runway bay, the Holding bay is there to drop on");
        rec.calls.clear();
        w.drag(s, card.x0 + 30, (card.y0 + card.y1) * 0.5f, bay.x0 + 60, bay.y0 + 8);
        check(rec.has("move 9 holding"), "dragging a strip onto another bay moves it there (" + rec.last() + ")");
        check(!rec.has("move 9 incoming"), "nothing is ever dropped into Incoming");
        // a strip the trainer says a controller moved shows in that bay, until the aircraft's state changes
        tower::State m = s;
        for (auto& a : m.ac) if (a.cs == "LOT789") { a.stripBay = "holding"; a.stripState = a.state; }
        w.frames(m, 2);
        const ProbeRect moved = probe("strip:LOT789"), hold = probe("bay:holding");
        check(moved.y0 >= hold.y0 && moved.y1 <= hold.y1 + 1, "the moved strip is drawn in the Holding bay");
    }
    // ---- radar: click to select, wheel zoom, drag pan, right-drag look, right-click menu
    {
        u = TowerUi();
        Win w(kTwRadar, 960, 620);
        w.frames(s, 3);
        const ProbeRect afr = probe("ac:AFR1752"), scope = probe("radar");
        check(afr.found && scope.found, "the radar draws AFR1752 (vacating at A2) inside the scope");
        rec.calls.clear();
        w.click(s, (afr.x0 + afr.x1) * 0.5f, (afr.y0 + afr.y1) * 0.5f);
        check(rec.has("select 6") && std::string(u.cmd) == "AFR1752 ", "clicking an aircraft selects it");
        rec.calls.clear();
        w.click(s, scope.x0 + 6, scope.y0 + 6);
        check(rec.has("select -1"), "clicking empty radar clears the selection");
        const float ppm0 = u.view.ppm;
        w.wheel(s, (scope.x0 + scope.x1) * 0.5f, (scope.y0 + scope.y1) * 0.5f, 2.f);
        check(u.view.ppm > ppm0 * 1.2f, "the wheel zooms in");
        const float cx0 = u.view.cx;
        w.drag(s, scope.x0 + 300, scope.y0 + 300, scope.x0 + 200, scope.y0 + 300);
        check(u.view.cx > cx0 + 1.f, "dragging moves the picture (the view follows the mouse)");
        rec.calls.clear();
        w.drag(s, scope.x0 + 300, scope.y0 + 300, scope.x0 + 340, scope.y0 + 300, 1);
        bool looked = false;
        for (const auto& c : rec.calls) if (c.rfind("look ", 0) == 0 && c.find("look -") != 0) looked = true;
        check(looked, "right-drag turns the camera (look right)");
        w.frames(s, 2);
        const ProbeRect d2 = probe("ac:AFR1752");
        rec.calls.clear();
        w.click(s, (d2.x0 + d2.x1) * 0.5f, (d2.y0 + d2.y1) * 0.5f, 1);
        w.frames(s, 2);
        check(probe("menu:follow").found, "right-click on an aircraft opens its menu");
        w.clickProbe(s, "menu:follow");
        check(rec.has("follow 6"), "Follow with the camera from the menu");
        tower::State sel = s;
        sel.selectedId = 1;
        rec.calls.clear();
        w.frames(sel, 2);
        check(w.clickProbe(sel, "follow") && rec.has("follow 1"), "the Follow button follows the selected aircraft");
        check(w.clickProbe(sel, "fit"), "Fit is there");
        w.frames(sel, 2);
        check(std::fabs(u.view.ppm - ppm0) < ppm0 * 0.02f, "Fit brings back the whole airport");
        // the label filter (TowerTrainer's): on the whole airport the service vehicles have no label and parked
        // aircraft their callsign at most, while FIN321, calling, has its whole label; the mouse on the bus shows its
        clearProbes();
        w.frames(s, 2);
        const ProbeRect fin = probe("label:FIN321"), busAc = probe("ac:BUS1");
        bool parkedShort = true;
        for (const char* cs : {"EZY12AB", "NSZ4UV", "TOM7PL", "WZZ3MX", "CLX74K"}) { const ProbeRect r = probe(std::string("label:") + cs); if (r.found && r.y1 - r.y0 > 20.f) parkedShort = false; }
        check(fin.found && fin.y1 - fin.y0 > 20.f && busAc.found && !probe("label:BUS1").found && !probe("label:FUEL2").found && parkedShort,
              "zoomed out: FIN321 (calling) has its whole label, the parked aircraft their callsign at most, the service vehicles none");
        w.move(s, (busAc.x0 + busAc.x1) * 0.5f, (busAc.y0 + busAc.y1) * 0.5f);
        w.frames(s, 2);
        check(probe("label:BUS1").found, "the mouse on the bus brings up its label");
        rec.calls.clear();
        check(w.clickProbe(sel, "popout") && rec.has("popout 0 out"), "the pop-out button asks for its own window");
    }
    // ---- operations: runway, LVP, stop bars, the light gun only with an aircraft selected
    {
        Win w(kTwOps, 640, 360);
        tower::State none = s; none.selectedId = -1;
        w.frames(none, 3);
        rec.calls.clear();
        w.clickProbe(none, "light:green steady");
        check(rec.calls.empty(), "the light gun does nothing with no aircraft selected");
        tower::State sel = s; sel.selectedId = 1;
        w.frames(sel, 2);
        w.clickProbe(sel, "light:green flashing");
        check(rec.has("light 1 green flashing"), "with FIN321 selected the light gun signals it");
        rec.calls.clear();
        check(w.clickProbe(sel, "lvp") && rec.has("lvp on"), "the LVP switch asks for LVP on");
        check(w.clickProbe(sel, "stopbars") && rec.has("stopbars off"), "the stop bars switch turns the lit bars off");
        w.clickProbe(sel, "runway");
        w.frames(sel, 2);
        check(w.clickProbe(sel, "rwy:08") && rec.has("runway 08"), "picking 08 in the runway list asks for runway 08");
        tower::State old = sel; old.haveStopBars = false;
        rec.calls.clear();
        w.frames(old, 2);
        check(w.clickProbe(old, "stopbars") && rec.has("stopbars on"), "an older trainer (no stop-bar state): on and off buttons instead");
    }
    // ---- the banner: one place that says what is missing, with Retry
    {
        Win w(kTwBanner, 640, 128);
        tower::State down; down.phase = tower::Phase::Unreachable; down.endpoint = "127.0.0.1:8781"; down.linkError = "connection refused"; down.retryInS = 4.f;
        const tw::Banner b = tw::bannerFor(down, "TOWER");
        check(b.show && b.retry && b.title == "Start TowerTrainer (server.py) \xE2\x80\x93 waiting for 127.0.0.1:8781\xE2\x80\xA6" && b.detail.find("4 s") != std::string::npos,
              "unreachable: \"" + b.title + "\" -- " + b.detail);
        w.frames(down, 3);
        rec.calls.clear();
        check(w.clickProbe(down, "retry") && rec.has("retry"), "Retry asks for the link now");
        tower::State nos; nos.phase = tower::Phase::NoSession; nos.endpoint = down.endpoint;
        check(tw::bannerFor(nos, "TOWER").detail.find("X-Plane tower link") != std::string::npos, "no session: says where to tick it in TowerTrainer");
        tower::State wait; wait.phase = tower::Phase::Linked; wait.linked = true;
        check(tw::bannerFor(wait, "TOWER").show && !tw::bannerFor(s, "TOWER").show, "linked without an airport: a banner; linked with one: none");
    }
    // ---- every window draws in both themes, small, and with nothing at all
    {
        tower::State empty;
        empty.phase = tower::Phase::Connecting;
        int drew = 0;
        for (const char* theme : {"dark", "light"}) {
            for (int i = 0; i < kTwCount; ++i) {
                for (const float sz : {1.f, 0.35f}) {
                    Win w(i, 900 * sz + 60, 600 * sz + 40, theme);
                    w.frames(s, 2);
                    w.frames(empty, 2);
                    ++drew;
                }
            }
        }
        check(drew == kTwCount * 4, "all six windows draw in dark and light, full size and squeezed, linked and with nothing linked");
    }
    // ---- the pure logic the windows lean on
    {
        tower::Aircraft a; a.kind = "dep"; a.state = "holding";
        check(tw::autoBay(a) == "holding" && tw::bayOf(a) == "holding", "a departure at the holding point is in Holding");
        a.stripBay = "runway"; a.stripState = "holding";
        check(tw::bayOf(a) == "runway", "moved by a controller: in that bay");
        a.state = "lineup";
        check(tw::bayOf(a) == "runway" && tw::autoBay(a) == "runway", "...until its state changes (then the state decides)");
        check(tw::withCallsign("", "FIN321") == "FIN321 " && tw::withCallsign("SAS123 ", "FIN321") == "FIN321 " && tw::withCallsign("SAS123 taxi to", "FIN321") == "SAS123 taxi to",
              "selecting puts the callsign in front, unless an instruction is half written");
        check(tw::displayText("\xF0\x9F\x92\xA1 tip \xE2\x9C\x93 done") == "tip OK done", "emoji dropped, a check mark in words");
        check(tw::levelText("4000") == "4000 ft" && tw::levelText("F180") == "FL180" && tw::waitingText(64) == "64 s" && tw::waitingText(200) == "3 min", "level and waiting text");
    }
    enableProbes(false);
    printf(fails ? "%d FAILED\n" : "tower_ui_test OK\n", fails);
    return fails ? 1 : 0;
}
