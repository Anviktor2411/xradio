// Drives the real plugin against a real server, without X-Plane.
//
//   ./plugin_harness <host> <port> <callsign>
//
// Flies a straight line, reports what the plugin's own window would display,
// and exits non-zero if the plugin failed to connect.
#include "harness.h"
#include "protocol.h"
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
    if (!harness::ensureTempDir()) return 1;
    const int wrongPort = atoi(port) + 1;
    FILE* f = fopen(harness::cfgPath().c_str(), "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", harness::cfgPath().c_str()); return 1; }
    fprintf(f, "host = %s\nport = %d\ncallsign = WRONG1\nactype = C172\n", host, wrongPort);
    fclose(f);

    double lat = 57.85, lon = 27.02;
    const double altM = 3000.0 / 3.28084;
    const float  gsMs = 60.0f;
    setPos(lat, lon, altM, 90.0f, gsMs);
    harness::set("sim/cockpit2/radios/actuators/com1_frequency_hz_833", 122800);
    harness::set("sim/cockpit2/radios/actuators/audio_selection_com1", 1);
    // A powered-up aircraft: the radios need the avionics bus, their own
    // switches and volts behind them, exactly as in the sim.
    harness::set("sim/cockpit2/switches/avionics_power_on", 1);
    harness::set("sim/cockpit2/radios/actuators/com1_power", 1);
    harness::set("sim/cockpit2/radios/actuators/com2_power", 1);
    harness::set("sim/cockpit2/electrical/bus_volts", 24.0);
    // Squawking mode C. TCAS interrogates with our own transponder, so with
    // this off there is no traffic display at all -- correctly, but it is not
    // what this harness is here to prove.
    harness::set("sim/cockpit2/radios/actuators/transponder_mode", xr::XPDR_ALT);
    harness::set("sim/cockpit2/radios/actuators/transponder_code", 1200);
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
    // The socket is opened on the network thread now, so give it a moment.
    std::vector<std::string> before;
    for (int i = 0; i < 40; ++i) {
        harness::tick(0.05f);
        before = harness::draw();
        if (harness::drawnContains(before, "logging in")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
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
        XPluginStop();
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
        FILE* cf = fopen(harness::cfgPath().c_str(), "r");
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

    // Pointed at a stand-in release endpoint, the window has to actually say so:
    // the check working but the line never appearing would be invisible.
    if (getenv("XRADIO_UPDATE_URL")) {
        bool said = false;
        for (const auto& l : harness::draw())
            if (l.find("is out") != std::string::npos) said = true;
        if (!said) {
            printf("FAIL: an update was available but the window never said so\n");
            return 1;
        }
        printf("the window offers the update\n");
    }

    // ---- v6: ATC. With XR_HARNESS_ATC set, a station (tools/fake_client.py
    // --atc ... --atis ... --ack-plans 4521) is on the server: the window must
    // list it, a click on it must tune COM1, the ATIS must show once its
    // frequency is tuned, and a flight plan filed on the ATC tab must come
    // back acknowledged with the squawk -- and be read out. ----
    bool atcOk = true;
    if (getenv("XR_HARNESS_ATC")) {
        const unsigned twrKhz = 135905, atisKhz = 127905;
        bool listed = false;
        for (int i = 0; i < 50 && !listed; ++i) {          // the list comes every 5 s at the latest
            harness::tick(0.2f);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            listed = harness::drawnContains(harness::draw(), "EETN_TWR");
        }
        if (!listed) { fprintf(stderr, "FAIL: the ATC station never appeared in the window\n"); atcOk = false; }
        int rx = 0, ry = 0;
        if (listed && harness::drawnAt("  EETN_TWR", &rx, &ry)) {
            harness::click(1, rx + 20, ry + 2);
            harness::tick(0.1f);
            const unsigned com1 = (unsigned)harness::get("sim/cockpit2/radios/actuators/com1_frequency_hz_833");
            if (com1 != twrKhz) { fprintf(stderr, "FAIL: clicking the station did not tune COM1 (%u)\n", com1); atcOk = false; }
            else printf("atc: clicking EETN_TWR tuned COM1 to %u\n", com1);
        } else if (listed) { fprintf(stderr, "FAIL: no clickable row for EETN_TWR\n"); atcOk = false; }

        harness::set("sim/cockpit2/radios/actuators/com2_frequency_hz_833", atisKhz);
        bool atis = false;
        for (int i = 0; i < 40 && !atis; ++i) {
            harness::tick(0.2f);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            atis = harness::drawnContains(harness::draw(), "ATIS EETN information");
        }
        if (!atis) { fprintf(stderr, "FAIL: the ATIS never showed after tuning its frequency\n"); atcOk = false; }
        else printf("atc: ATIS shown after tuning COM2 to it\n");

        // File a plan through the settings window's ATC tab, the way a pilot does.
        harness::menu(1);
        harness::drawWindow(2);
        int tx = 0, ty = 0;
        if (harness::drawnAt("ATC", &tx, &ty)) { harness::click(2, tx + 5, ty + 2); harness::drawWindow(2); }
        int wtop = 0, wleft = 0;
        harness::windowTop(2, &wtop, &wleft);
        auto typeInto = [&](const char* label, const char* value) {
            harness::drawWindow(2);
            int lx = 0, ly = 0;
            if (!harness::drawnAt(label, &lx, &ly)) return false;
            harness::click(2, wleft + 200, ly);
            harness::drawWindow(2);
            for (int i = 0; i < 20; ++i) harness::pressVk(2, 0x08);
            harness::typeText(2, value);
            harness::drawWindow(2);
            return true;
        };
        if (!typeInto("Departure (ICAO)", "EETN") || !typeInto("Destination", "EFHK")) {
            fprintf(stderr, "FAIL: the flight plan fields were not found on the ATC tab\n"); atcOk = false;
        }
        int bx = 0, by = 0;
        if (harness::drawnAt("File flight plan", &bx, &by)) { harness::click(2, bx + 10, by + 2); harness::drawWindow(2); }
        else { fprintf(stderr, "FAIL: no 'File flight plan' button on the ATC tab\n"); atcOk = false; }
        auto sw2 = harness::drawWindow(2);
        if (!harness::drawnContains(sw2, "Flight plan filed")) {
            fprintf(stderr, "FAIL: filing the plan gave no confirmation\n");
            for (auto& l : sw2) fprintf(stderr, "   %s\n", l.c_str());
            atcOk = false;
        }
        harness::pressVk(2, 0x1B);                             // Escape closes the window
        harness::drawWindow(2);
        bool acked = false;
        for (int i = 0; i < 40 && !acked; ++i) {
            harness::tick(0.2f);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            acked = harness::drawnContains(harness::draw(), "squawk 4521");
        }
        if (!acked) { fprintf(stderr, "FAIL: the station's acknowledgement (squawk 4521) never showed\n"); atcOk = false; }
        else printf("atc: flight plan acknowledged with squawk 4521\n");
        bool spoken = false;
        for (const auto& l : harness::log())
            if (l.rfind("speak: ", 0) == 0 && l.find("4521") != std::string::npos) spoken = true;
        if (!spoken) { fprintf(stderr, "FAIL: the acknowledgement was not read out\n"); atcOk = false; }
        printf("\n--- the window with ATC in the flight ---\n");
        for (const auto& l : harness::draw()) printf("  %s\n", l.c_str());
    }

    XPluginDisable();
    XPluginStop();

    if (!connected) { fprintf(stderr, "FAIL: never connected\n"); return 1; }
    if (!sawTraffic) { fprintf(stderr, "FAIL: never saw traffic\n"); return 1; }
    if (!sawChat)    { fprintf(stderr, "FAIL: never received a radio message\n"); return 1; }
    if (!voiceOk)    { fprintf(stderr, "FAIL: voice round trip\n"); return 1; }
    if (!atcOk)      { fprintf(stderr, "FAIL: ATC\n"); return 1; }
    printf("harness OK\n");
    return 0;
}
