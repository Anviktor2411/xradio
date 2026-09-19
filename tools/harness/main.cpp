// Drives the real plugin against a real server, without X-Plane.
//
//   ./plugin_harness <host> <port> <callsign>
//
// Flies a straight line, reports what the plugin's own window would display,
// and exits non-zero if the plugin failed to connect.
#include "harness.h"
#include "voice.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include "mathconst.h"

extern "C" {
int  XPluginStart(char*, char*, char*);
int  XPluginEnable(void);
void XPluginDisable(void);
void XPluginStop(void);
}

static void setPos(double lat, double lon, double altM, float hdg, float gsMs) {
    harness::set("sim/flightmodel/position/latitude", lat);
    harness::set("sim/flightmodel/position/longitude", lon);
    harness::set("sim/flightmodel/position/elevation", altM);
    harness::set("sim/flightmodel/position/psi", hdg);
    harness::set("sim/flightmodel/position/groundspeed", gsMs);
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const char* port = argc > 2 ? argv[2] : "49100";
    const char* cs   = argc > 3 ? argv[3] : "HARNES";
    harness::g_verbose = (getenv("XR_VERBOSE") != nullptr);

    // The plugin writes its config next to the prefs path the stub reports.
    // Start it pointed at the WRONG port on purpose: the settings window is
    // then the only thing that can make the connection succeed, so the
    // connect check below also proves the settings path works end to end.
    if (system("mkdir -p /tmp/xradio-harness") != 0) return 1;
    const int wrongPort = atoi(port) + 1;
    FILE* f = fopen("/tmp/xradio-harness/xradio.cfg", "w");
    fprintf(f, "host = %s\nport = %d\ncallsign = WRONG1\nactype = C172\n", host, wrongPort);
    fclose(f);

    double lat = 57.85, lon = 27.02;
    const double altM = 3000.0 / 3.28084;
    const float  gsMs = 60.0f;
    setPos(lat, lon, altM, 90.0f, gsMs);
    harness::set("sim/cockpit2/radios/actuators/com1_frequency_hz_833", 122800);
    harness::set("sim/cockpit2/radios/actuators/audio_selection_com1", 1);
    harness::set("sim/cockpit2/radios/actuators/audio_com_selection", 6);
    harness::set("sim/cockpit2/switches/navigation_lights_on", 1);

    char name[256], sig[256], desc[256];
    if (!XPluginStart(name, sig, desc)) {
        fprintf(stderr, "XPluginStart failed\n");
        return 1;
    }
    printf("started: %s (%s)\n", name, sig);

    if (!XPluginEnable()) {
        fprintf(stderr, "XPluginEnable failed\n");
        return 1;
    }

    // ---- settings window: fix the port and callsign through the UI ----
    harness::tick(0.2f);
    auto before = harness::draw();
    if (!harness::drawnContains(before, "logging in")) {
        fprintf(stderr, "FAIL: expected to be logging in (to the wrong port) at start\n");
        return 1;
    }

    harness::menu(1);                                   // Plugins > XRadio > Settings...
    if (!harness::windowVisible(2)) {
        fprintf(stderr, "FAIL: settings window did not open\n");
        return 1;
    }
    auto sw = harness::drawWindow(2);
    if (!harness::drawnContains(sw, "Server host") || !harness::drawnContains(sw, "Port")) {
        fprintf(stderr, "FAIL: settings window is missing its fields\n");
        return 1;
    }

    int top = 0, left = 0;
    harness::windowTop(2, &top, &left);

    // Click the row a label was actually drawn on. A click only takes focus
    // on the frame it is processed, so the field is redrawn before anything
    // is typed into it -- exactly the order the sim produces.
    auto focusField = [&](const char* label) {
        harness::drawWindow(2);
        int lx = 0, ly = 0;
        if (!harness::drawnAt(label, &lx, &ly)) {
            fprintf(stderr, "FAIL: no '%s' row in the settings window\n", label);
            exit(1);
        }
        harness::click(2, left + 200, ly);
        harness::drawWindow(2);
    };

    // Port field: click it, wipe it, type the right one.
    focusField("Port");
    for (int i = 0; i < 8; ++i) harness::pressVk(2, 0x08);   // XPLM_VK_BACK
    harness::typeText(2, port);
    sw = harness::drawWindow(2);
    if (!harness::drawnContains(sw, std::string("> ") + port)) {
        fprintf(stderr, "FAIL: typed port not shown in the field\n");
        for (auto& l : sw) fprintf(stderr, "   %s\n", l.c_str());
        return 1;
    }

    // Callsign: Tab to it (from port), wipe, type ours in lower case --
    // the plugin should upper-case it on save.
    harness::pressVk(2, 0x09);                                // XPLM_VK_TAB
    harness::drawWindow(2);                                   // Tab takes effect
    for (int i = 0; i < 20; ++i) harness::pressVk(2, 0x08);
    std::string lower = cs;
    for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
    harness::typeText(2, lower);
    sw = harness::drawWindow(2);
    if (!harness::drawnContains(sw, std::string("> ") + lower)) {
        fprintf(stderr, "FAIL: Tab did not move focus to the callsign field\n");
        for (auto& l : sw) fprintf(stderr, "   %s\n", l.c_str());
        return 1;
    }

    // Validation: an empty host must be refused, not saved.
    focusField("Server host");
    for (int i = 0; i < 70; ++i) harness::pressVk(2, 0x08);
    harness::pressVk(2, 0x0D);                                // XPLM_VK_RETURN
    harness::drawWindow(2);                                   // Enter acts on draw
    if (!harness::windowVisible(2)) {
        fprintf(stderr, "FAIL: settings accepted an empty host\n");
        return 1;
    }
    sw = harness::drawWindow(2);
    if (!harness::drawnContains(sw, "Host cannot be empty")) {
        fprintf(stderr, "FAIL: no validation message for empty host\n");
        return 1;
    }
    harness::typeText(2, host);
    harness::pressVk(2, 0x0D);                                // save & reconnect
    harness::drawWindow(2);

    if (harness::windowVisible(2)) {
        fprintf(stderr, "FAIL: settings window stayed open after save\n");
        return 1;
    }
    // The saved file must carry the corrected values, callsign upper-cased.
    {
        FILE* cf = fopen("/tmp/xradio-harness/xradio.cfg", "r");
        std::string all;
        char buf[256];
        while (cf && fgets(buf, sizeof(buf), cf)) all += buf;
        if (cf) fclose(cf);
        const std::string wantPort = std::string("port = ") + port;
        const std::string wantCs   = std::string("callsign = ") + cs;
        if (all.find(wantPort) == std::string::npos || all.find(wantCs) == std::string::npos) {
            fprintf(stderr, "FAIL: config on disk not updated:\n%s", all.c_str());
            return 1;
        }
    }
    printf("settings: port fixed through the UI, callsign upper-cased, empty host refused\n");

    bool connected = false;
    bool sawTraffic = false;
    bool sawChat = false;

    // Tick at real-time pace: the plugin's timeouts are wall-clock based,
    // and the peer on the other end of the server talks in real seconds too.
    for (int i = 0; i < 70; ++i) {           // 70 * 0.2 s = 14 s
        // dead-reckon east so the server sees us moving
        lon += (gsMs * 0.2) / (111320.0 * cos(lat * xr::kPi / 180.0));
        setPos(lat, lon, altM, 90.0f, gsMs);

        if (i == 20) harness::ptt(true);      // key for 2 s: 100 voice frames
        if (i == 30) harness::ptt(false);
        if (i == 32) harness::menu(4);        // "Send test message"

        harness::tick(0.2f);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto lines = harness::draw();
        if (harness::drawnContains(lines, "connected to")) connected = true;
        if (harness::drawnContains(lines, "Traffic (1)") ||
            harness::drawnContains(lines, "Traffic (2)")) sawTraffic = true;
        for (const auto& l : lines) {
            if (l.find("122.800") != std::string::npos &&
                l.find(":") != std::string::npos &&
                l.find("COM1") == std::string::npos) {
                sawChat = true;
            }
        }

        // Snapshot the window once while keyed (mic meter, RX line) and once
        // at the end. Never bail out early: the PTT window must run its course.
        if (i == 27 || i == 69) {
            printf("\n--- what the plugin window shows (t=%.1fs) ---\n", i * 0.2);
            for (const auto& l : lines) printf("  %s\n", l.c_str());
        }
    }

    printf("\nconnected=%d traffic=%d chat=%d\n", connected, sawTraffic, sawChat);

    // ---- voice: did keying the PTT actually put frames on the wire, and did
    // the peer's echo come back through our decoder? ----
    std::this_thread::sleep_for(std::chrono::milliseconds(500));   // let the echo land
    const auto vs = xr::voice::stats();
    printf("voice: %s\n", xr::voice::status().c_str());
    printf("voice: encoded=%llu received=%llu played=%llu concealed=%llu\n",
           (unsigned long long)vs.framesEncoded, (unsigned long long)vs.framesReceived,
           (unsigned long long)vs.framesPlayed, (unsigned long long)vs.framesConcealed);
    bool voiceOk = true;
    if (!xr::voice::available()) {
        fprintf(stderr, "FAIL: voice pipeline did not initialise\n");
        voiceOk = false;
    }
    if (vs.framesEncoded < 50) {
        fprintf(stderr, "FAIL: PTT held for 2 s should encode ~100 frames, got %llu\n",
                (unsigned long long)vs.framesEncoded);
        voiceOk = false;
    }
    if (vs.framesReceived < 20) {
        fprintf(stderr, "FAIL: the peer echoed our voice but only %llu frames came back\n",
                (unsigned long long)vs.framesReceived);
        voiceOk = false;
    }
    if (vs.framesPlayed < 5) {
        fprintf(stderr, "FAIL: echoed frames were received but never played (%llu)\n",
                (unsigned long long)vs.framesPlayed);
        voiceOk = false;
    }

    XPluginDisable();
    XPluginStop();

    if (!connected) { fprintf(stderr, "FAIL: never connected\n"); return 1; }
    if (!sawTraffic) { fprintf(stderr, "FAIL: never saw traffic\n"); return 1; }
    if (!sawChat)    { fprintf(stderr, "FAIL: never received a radio message\n"); return 1; }
    if (!voiceOk)    { fprintf(stderr, "FAIL: voice round trip\n"); return 1; }
    printf("harness OK\n");
    return 0;
}
