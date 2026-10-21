// The Dear ImGui window's Apply, the way it crashed X-Plane in v0.6.0 and must not again: the real plugin with
// its real ImGui windows (headless: every draw list is read, nothing is rendered), connected to a fake server,
// changes the callsign, the port, the UI scale, the theme, the language, the audio devices and hosting and
// presses Apply -- on the Settings tab, on Host / Connect, through the wizard -- fifty times over, with the
// cockpit overlay on and, when XR_TOWER_PORT names a running tools/fake_towertrainer.py, Tower mode's windows
// open. After every frame the current ImGui context is what it was before, no window is left mid-frame, no
// IM_ASSERT failed, the settings reached the file, the fonts were rebuilt between frames and not inside the
// click, and each change reconnected exactly once -- from the flight loop, never from inside the frame.
//   ./ui_apply_test [port]                       (XR_TOWER_PORT=8781 with the fake trainer for the tower windows)
#include "harness.h"
#include "protocol.h"
#include "settings.h"
#include "tower.h"
#include "ui2/app_ui.h"
#include "ui2/imgui_assert.h"
#include "ui2/imgui_xp.h"
#include "ui2/tower_ui.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define CLOSESOCK ::close
#endif

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

// Windows in creation order: the text main window, its settings, the notices (never shown in this build), then
// the ImGui main window, the overlay, and Tower mode's six.
static constexpr int kMain = 4, kOverlay = 5, kTower0 = 6, kTowerN = 6;

// ---------------------------------------------------------------------------
// A server that answers logins, keeps the connection alive, and counts.
// ---------------------------------------------------------------------------
class FakeServer {
public:
    explicit FakeServer(uint16_t port) {
        fd_ = (int)socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
        DWORD tv = 100;
#else
        timeval tv{0, 100 * 1000};
#endif
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_port = htons(port);
        me.sin_addr.s_addr = inet_addr("127.0.0.1");
        ok_ = ::bind(fd_, (const sockaddr*)&me, sizeof(me)) == 0;
        if (ok_) worker_ = std::thread(&FakeServer::run, this);
    }
    ~FakeServer() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
        if (fd_ >= 0) CLOSESOCK(fd_);
    }
    bool ok() const { return ok_; }
    int  logins() const { return logins_.load(); }
    int  logouts() const { return logouts_.load(); }
    std::string lastCallsign() { std::lock_guard<std::mutex> lk(mx_); return lastCs_; }

private:
    void run() {
        uint8_t buf[xr::kMaxPacket];
        while (!stop_) {
            sockaddr_in from{};
#ifdef _WIN32
            int fromLen = (int)sizeof(from);
#else
            socklen_t fromLen = sizeof(from);
#endif
            const int n = (int)recvfrom(fd_, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
            if (n < (int)sizeof(xr::Header)) continue;
            xr::Header h{};
            memcpy(&h, buf, sizeof(h));
            if (h.magic != xr::kMagic) continue;
            std::lock_guard<std::mutex> lk(mx_);
            peer_ = from;
            if (h.type == xr::PT_LOGIN && n >= (int)(sizeof(h) + sizeof(xr::LoginPayload))) {
                xr::LoginPayload lp{};
                memcpy(&lp, buf + sizeof(h), sizeof(lp));
                lastCs_ = std::string(lp.callsign, strnlen(lp.callsign, sizeof lp.callsign));
                ++logins_;
                xr::LoginAckPayload ack{};
                ack.sessionId = ++sid_;
                send(xr::PT_LOGIN_ACK, &ack, (int)sizeof(ack));
            } else if (h.type == xr::PT_LOGOUT) {
                ++logouts_;
            } else if (h.type == xr::PT_PING) {
                send(xr::PT_PONG, nullptr, 0);
            } else if (h.type == xr::PT_POSITION) {
                xr::TrafficHeader th{};          // a quiet server is dropped after 12 s; this one answers
                send(xr::PT_TRAFFIC, &th, (int)sizeof(th));
            }
        }
    }
    void send(uint8_t type, const void* payload, int len) {
        uint8_t out[xr::kMaxPacket];
        xr::Header h{};
        h.magic = xr::kMagic; h.type = type; h.version = (uint8_t)xr::kProtoVersion; h.payloadLen = (uint16_t)len; h.sessionId = sid_;
        memcpy(out, &h, sizeof(h));
        if (len) memcpy(out + sizeof(h), payload, (size_t)len);
        sendto(fd_, (const char*)out, sizeof(h) + (size_t)len, 0, (const sockaddr*)&peer_, sizeof(peer_));
    }

    int fd_ = -1;
    bool ok_ = false;
    uint32_t sid_ = 10;
    std::atomic<bool> stop_{false};
    std::atomic<int> logins_{0}, logouts_{0};
    std::thread worker_;
    std::mutex mx_;
    sockaddr_in peer_{};
    std::string lastCs_;
};

// ---------------------------------------------------------------------------
// driving the ImGui windows
// ---------------------------------------------------------------------------
static ImGuiContext* g_sentinel = nullptr;   // the test's own context: current before and after every callback
static int g_contextSlips = 0, g_midFrame = 0;

static void drawVisible() {
    for (int id = kMain; id < kTower0 + kTowerN && id <= harness::windowCount(); ++id) {
        if (!harness::windowVisible(id)) continue;
        harness::drawWindow(id);
        if (ImGui::GetCurrentContext() != g_sentinel) { ++g_contextSlips; ImGui::SetCurrentContext(g_sentinel); }
        if (xr::ui2::framesInFlight() != 0) ++g_midFrame;
    }
}

// The flight loop, the windows, real time passing: what X-Plane does between two clicks.
static void fly(double seconds) {
    const int steps = (int)(seconds / 0.05) + 1;
    for (int i = 0; i < steps; ++i) {
        harness::tick(0.05f);
        if (ImGui::GetCurrentContext() != g_sentinel) { ++g_contextSlips; ImGui::SetCurrentContext(g_sentinel); }
        drawVisible();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// A probed control of the main window, clicked the way X-Plane delivers a click: the cursor over it, the button
// down, a frame, the button up, a frame (ImGui acts on the release).
static bool clickProbe(const std::string& name) {
    harness::drawWindow(kMain);
    const xr::ui2::ProbeRect r = xr::ui2::probe(name);
    if (!r.found) return false;
    int l, t, rr, b;
    harness::windowRect(kMain, &l, &t, &rr, &b);
    const int x = l + (int)((r.x0 + r.x1) * 0.5f), y = t - (int)((r.y0 + r.y1) * 0.5f);
    harness::hover(kMain, x, y);
    harness::mouseButton(kMain, x, y, true);
    drawVisible();
    harness::mouseButton(kMain, x, y, false);
    drawVisible();
    drawVisible();
    if (ImGui::GetCurrentContext() != g_sentinel) { ++g_contextSlips; ImGui::SetCurrentContext(g_sentinel); }
    return true;
}

static int countLog(const std::string& needle) {
    int n = 0;
    for (const auto& l : harness::log()) if (l.find(needle) != std::string::npos) ++n;
    return n;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    const uint16_t port = (uint16_t)(argc > 1 ? atoi(argv[1]) : 49720);
    const uint16_t hostPort = (uint16_t)(port + 2);
    harness::g_verbose = getenv("XR_VERBOSE") != nullptr;
    if (!harness::ensureTempDir()) return 1;
    harness::setEnv("XRADIO_UPNP_GATEWAY", "127.0.0.1");   // no router is asked
    harness::setEnv("XRADIO_SSDP_PORT", "11903");
    const char* towerPort = getenv("XR_TOWER_PORT");

    printf("the context guard\n");
    {
        g_sentinel = ImGui::CreateContext();
        ImGuiContext* a = ImGui::CreateContext();
        ImGuiContext* b = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_sentinel);
        {
            xr::ui2::ContextGuard ga(a);
            check("a guard switches to its context", ImGui::GetCurrentContext() == a);
            {
                xr::ui2::ContextGuard gb(b);
                check("nested: the inner one", ImGui::GetCurrentContext() == b);
            }
            check("nested: back to the outer one", ImGui::GetCurrentContext() == a);
        }
        check("after the guard the previous context is current again", ImGui::GetCurrentContext() == g_sentinel);
        bool caught = false;
        try {
            xr::ui2::ContextGuard gc(b);
            throw std::runtime_error("drawing failed");
        } catch (const std::exception&) { caught = true; }
        check("an exception through a guard restores it too", caught && ImGui::GetCurrentContext() == g_sentinel);
        {
            xr::ui2::ContextGuard gn(nullptr);
            check("a guard may switch to no context", ImGui::GetCurrentContext() == nullptr);
        }
        check("...and comes back from that", ImGui::GetCurrentContext() == g_sentinel);
        ImGui::DestroyContext(a);
        ImGui::DestroyContext(b);
        ImGui::SetCurrentContext(g_sentinel);
    }

    printf("\nthe plugin against a fake server\n");
    FakeServer srv(port);
    check("the fake server is listening", srv.ok(), "port " + std::to_string(port) + " in use?");
    if (!srv.ok()) return 1;
    {
        xr::Settings s;
        s.callsign = "TESTER"; s.acIcao = "C172"; s.host = "127.0.0.1"; s.port = std::to_string(port);
        s.hostEnabled = false; s.hostPort = std::to_string(hostPort); s.hostUpnp = false; s.autoConnect = true;
        s.uiWizardDone = true; s.uiOverlay = true; s.uiScale = 1.f; s.uiTheme = "dark"; s.uiLang = "en";
        if (towerPort) s.towerPort = towerPort;      // Apply keeps the link as it is
        xr::saveSettings(s, harness::cfgPath());
    }
    harness::set("sim/cockpit2/radios/actuators/com1_frequency_hz_833", 122800);
    harness::set("sim/cockpit2/switches/avionics_power_on", 1);
    harness::set("sim/cockpit2/radios/actuators/com1_power", 1);
    harness::set("sim/cockpit2/radios/actuators/com2_power", 1);
    harness::set("sim/cockpit2/electrical/bus_volts", 24.0);
    harness::set("sim/cockpit2/radios/actuators/transponder_mode", xr::XPDR_ALT);
    harness::set("sim/flightmodel/position/latitude", 59.41);
    harness::set("sim/flightmodel/position/longitude", 24.83);
    harness::setArray("sim/flightmodel/position/q", {1.f, 0.f, 0.f, 0.f});
    harness::setArray("sim/operation/override/override_planepath", {0.f});

    char name[256], sig[256], desc[256];
    ImGui::SetCurrentContext(g_sentinel);
    check("XPluginStart", XPluginStart(name, sig, desc) == 1);
    check("creating the windows left the test's context current", ImGui::GetCurrentContext() == g_sentinel);
    check("the windows exist: text ones, the ImGui main window, the overlay, six tower windows", harness::windowCount() >= kTower0 + kTowerN - 1,
          std::to_string(harness::windowCount()) + " windows");
    check("XPluginEnable", XPluginEnable() == 1);
    fly(1.0);
    check("connected to the fake server", srv.logins() >= 1 && countLog("connected, session") >= 1, std::to_string(srv.logins()) + " logins");
    check("the overlay is up", harness::windowVisible(kOverlay));
    harness::menu(0);                        // Plugins > XRadio > Show / hide window
    check("the menu opens the ImGui window", harness::windowVisible(kMain));
    xr::ui2::enableProbes(true);

    if (towerPort) {
        xr::tower::toggle();
        const xr::tower::State& st = xr::tower::state();
        for (int i = 0; i < 100 && !(st.linked && st.haveAirport && !st.ac.empty() && st.phase == xr::tower::Phase::Linked); ++i) fly(0.1);
        check("Tower mode linked to the fake trainer", st.linked && st.haveAirport && st.phase == xr::tower::Phase::Linked, st.linkStatus);
        fly(0.5);
        int open = 0;
        for (int i = kTower0; i < kTower0 + kTowerN; ++i) if (harness::windowVisible(i)) ++open;
        check("the five tower work windows are open", open >= 5, std::to_string(open) + " open");
    } else {
        printf("  (XR_TOWER_PORT not set: Tower mode's windows stay closed; run tools/fake_towertrainer.py for them)\n");
    }

    printf("\nfifty Applies\n");
    xr::ui2::UiState& ui = xr::ui2::state();
    int slips0 = g_contextSlips, mid0 = g_midFrame;
    int bad = 0;
    bool hostingOn = false;
    for (int i = 0; i < 50; ++i) {
        const int kind = i % 5;              // what changes this time
        const bool onHostTab = i % 2 == 1;
        const bool viaWizard = i == 24;      // the wizard's Finish applies too
        xr::Settings before;
        xr::loadSettings(before, harness::cfgPath());
        if (viaWizard) { ui.wizardOpen = true; ui.wizard.step = xr::ui2::Wizard::kSteps - 1; }
        else if (!clickProbe(onHostTab ? "tab:5" : "tab:6")) { check("tab button found", false, "iteration " + std::to_string(i)); break; }
        if (!onHostTab && !viaWizard) clickProbe("group:" + std::to_string(kind == 0 ? 2 : kind == 1 ? 4 : kind == 2 ? 0 : 2));
        // the edit, as the widgets would have left it
        ui.editLoaded = true; ui.edit = before; ui.editDirty = true;
        bool netChange = false, fontChange = false, devChange = false;
        switch (kind) {
            case 0: ui.edit.uiScale = before.uiScale > 1.2f ? 1.f : 1.5f; fontChange = true; break;
            case 1: ui.edit.uiTheme = before.uiTheme == "dark" ? "light" : "dark"; ui.edit.uiLang = before.uiLang == "en" ? "et" : "en"; break;
            case 2: ui.edit.micDevice = before.micDevice.empty() ? "No such microphone" : ""; ui.edit.outDevice = before.outDevice.empty() ? "No such output" : ""; devChange = true; break;
            case 3: ui.edit.callsign = "TEST" + std::to_string(i); netChange = true; break;
            default: hostingOn = !hostingOn; ui.edit.hostEnabled = hostingOn; netChange = true; break;   // our own server, then the fake one again
        }
        if (!hostingOn && kind != 4 && i % 10 == 8) { ui.edit.port = std::to_string(port); }   // the same port typed again: no reconnect
        const int reconnects0 = countLog("XRadio: reconnecting to"), logins0 = srv.logins(), logouts0 = srv.logouts(), audio0 = countLog("audio device");
        const int writeFails0 = countLog("could not write");
        // press Apply: inside the window's frame nothing but a note is made
        bool pressed;
        if (viaWizard) { harness::drawWindow(kMain); pressed = clickProbe("wizard:finish"); }
        else pressed = clickProbe(onHostTab ? "apply:host" : "apply:settings");
        if (!pressed) { check("Apply button found", false, "iteration " + std::to_string(i)); break; }
        xr::Settings onDisk;
        xr::loadSettings(onDisk, harness::cfgPath());
        bool ok = true;
        std::string why;
        if (onDisk.callsign != before.callsign || onDisk.uiScale != before.uiScale || onDisk.hostEnabled != before.hostEnabled) { ok = false; why += "saved inside the frame; "; }
        if (countLog("XRadio: reconnecting to") != reconnects0) { ok = false; why += "reconnected inside the frame; "; }
        if (fontChange && !xr::ui2::fontRebuildPending()) { /* the flight loop has not run yet: nothing asked for */ }
        // the flight loop does the work
        harness::tick(0.05f);
        if (fontChange && !xr::ui2::fontRebuildPending()) { ok = false; why += "fonts not marked for a rebuild between frames; "; }
        drawVisible();
        if (fontChange && xr::ui2::fontRebuildPending()) { ok = false; why += "fonts not rebuilt at the next frame; "; }
        fly(netChange ? 1.2 : 0.3);
        xr::loadSettings(onDisk, harness::cfgPath());
        if (onDisk.callsign != ui.edit.callsign || onDisk.uiScale != ui.edit.uiScale || onDisk.uiTheme != ui.edit.uiTheme || onDisk.uiLang != ui.edit.uiLang ||
            onDisk.hostEnabled != ui.edit.hostEnabled || onDisk.micDevice != ui.edit.micDevice) { ok = false; why += "not saved; "; }
        const int reconnects = countLog("XRadio: reconnecting to") - reconnects0;
        if (reconnects != (netChange ? 1 : 0)) { ok = false; why += "reconnected " + std::to_string(reconnects) + " times; "; }
        if (netChange && !hostingOn && kind != 4) {
            if (srv.logins() - logins0 != 1) { ok = false; why += std::to_string(srv.logins() - logins0) + " logins at the fake server; "; }
            if (srv.logouts() - logouts0 != 1) { ok = false; why += std::to_string(srv.logouts() - logouts0) + " logouts; "; }
            if (srv.lastCallsign() != ui.edit.callsign) { ok = false; why += "logged in as " + srv.lastCallsign() + "; "; }
        }
        if (kind == 4) {
            if (hostingOn && !countLog("hosting on port " + std::to_string(hostPort))) { ok = false; why += "the built-in server did not start; "; }
            if (hostingOn && countLog("connected, session") < 2) { ok = false; why += "not connected to the built-in server; "; }
        }
        if (devChange && countLog("audio device") == audio0) { ok = false; why += "the audio devices were not reopened; "; }
        if (ui.editDirty) { ok = false; why += "Apply still pending; "; }
        if (countLog("could not write") != writeFails0) { ok = false; why += "the settings file could not be written; "; }
        if (!ok) ++bad;
        if (!ok || harness::g_verbose) printf("  %s  iteration %d (%s, %s)%s%s\n", ok ? "ok  " : "FAIL", i, onHostTab ? "Host / Connect" : viaWizard ? "wizard" : "Settings",
                                              kind == 0 ? "UI scale" : kind == 1 ? "theme + language" : kind == 2 ? "audio devices" : kind == 3 ? "callsign" : "hosting",
                                              why.empty() ? "" : ": ", why.c_str());
    }
    check("50 Applies: settings saved, fonts rebuilt between frames, one reconnect per change, nothing done inside the frame", bad == 0, std::to_string(bad) + " bad");
    check("the current ImGui context was never left changed after a frame or a click", g_contextSlips == slips0, std::to_string(g_contextSlips - slips0) + " slips");
    check("no window was left in the middle of a frame", g_midFrame == mid0);
    check("no IM_ASSERT failed", xr::ui2::imguiAssertCount() == 0, std::to_string(xr::ui2::imguiAssertCount()) + " failed");
    check("no window drawing failed", countLog("window drawing failed") == 0);
    check("no apply failed", countLog("apply failed") == 0);
    if (hostingOn) {                          // back to the fake server for the rest
        clickProbe("tab:5");
        ui.editLoaded = true; xr::loadSettings(ui.edit, harness::cfgPath()); ui.edit.hostEnabled = false; ui.editDirty = true;
        clickProbe("apply:host");
        fly(1.5);
    }

    printf("\nthe rest of the window\n");
    {
        // Reconnect with nothing changed: once, from the flight loop
        clickProbe("tab:5");
        ui.editLoaded = true; xr::loadSettings(ui.edit, harness::cfgPath()); ui.edit.port = std::to_string(port); ui.edit.hostEnabled = false; ui.editDirty = false;
        const int r0 = countLog("XRadio: reconnecting to");
        clickProbe("apply:host");             // reads "Reconnect" now
        check("Reconnect does nothing inside the frame", countLog("XRadio: reconnecting to") == r0);
        fly(1.2);
        check("...and reconnects once from the flight loop", countLog("XRadio: reconnecting to") == r0 + 1 && countLog("connected, session") >= 2);
        // settings that do not validate are refused, not saved, and the Apply stays pressable
        xr::Settings before;
        xr::loadSettings(before, harness::cfgPath());
        clickProbe("tab:6");
        ui.edit = before; ui.edit.callsign = ""; ui.editDirty = true;
        clickProbe("apply:settings");
        fly(0.3);
        xr::Settings after;
        xr::loadSettings(after, harness::cfgPath());
        check("an empty callsign is refused from the flight loop: not saved, Apply pending again", after.callsign == before.callsign && ui.editDirty && countLog("settings not saved") == 1);
        ui.edit = before; ui.editDirty = false;
        // the overlay off and on through Apply: the window hides and comes back, drawn in its own context
        ui.edit.uiOverlay = false; ui.editDirty = true; clickProbe("apply:settings"); fly(0.3);
        check("overlay off through Apply", !harness::windowVisible(kOverlay));
        ui.edit.uiOverlay = true; ui.editDirty = true; clickProbe("apply:settings"); fly(0.3);
        check("overlay on again", harness::windowVisible(kOverlay));
        // the same window reopened 20 times, applying each time: no drift
        for (int i = 0; i < 20; ++i) { harness::menu(0); fly(0.05); harness::menu(0); fly(0.05); ui.edit.uiScale = i % 2 ? 1.25f : 1.f; ui.editDirty = true; clickProbe("apply:settings"); fly(0.1); }
        check("twenty hide / show / Apply rounds: contexts and frames clean", g_contextSlips == slips0 && g_midFrame == mid0 && xr::ui2::imguiAssertCount() == 0);
    }

    if (towerPort) {
        printf("\nthe tower windows while applying\n");
        const int slips1 = g_contextSlips;
        for (int i = 0; i < 10; ++i) {
            for (int w = kTower0; w < kTower0 + kTowerN; ++w) if (harness::windowVisible(w)) { int l, t, r, b; harness::windowRect(w, &l, &t, &r, &b); harness::hover(w, (l + r) / 2, (t + b) / 2); harness::wheel(w, (l + r) / 2, (t + b) / 2, 1); }
            clickProbe("tab:6");
            ui.edit.uiTheme = i % 2 ? "light" : "dark"; ui.edit.uiScale = i % 3 ? 1.f : 1.4f; ui.editDirty = true;
            clickProbe("apply:settings");
            fly(0.3);
        }
        check("ten Applies with the tower windows open: contexts clean, no asserts", g_contextSlips == slips1 && xr::ui2::imguiAssertCount() == 0 && xr::tower::active());
        // a pop-out asked for from inside a tower window's frame happens on the flight loop
        check("tower windows are not popped out", !harness::windowPoppedOut(kTower0));
        xr::tower::leave();
        fly(0.3);
    }

    xr::ui2::enableProbes(false);
    XPluginDisable();
    XPluginStop();
    ImGui::SetCurrentContext(g_sentinel);
    ImGui::DestroyContext(g_sentinel);
    printf("\n%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
