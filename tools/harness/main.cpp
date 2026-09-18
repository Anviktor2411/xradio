// Drives the real plugin against a real server, without X-Plane.
//
//   ./plugin_harness <host> <port> <callsign>
//
// Flies a straight line, reports what the plugin's own window would display,
// and exits non-zero if the plugin failed to connect.
#include "harness.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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
    system("mkdir -p /tmp/xradio-harness");
    FILE* f = fopen("/tmp/xradio-harness/xradio.cfg", "w");
    fprintf(f, "host = %s\nport = %s\ncallsign = %s\nactype = C172\n", host, port, cs);
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

    bool connected = false;
    bool sawTraffic = false;
    bool sawChat = false;

    // Tick at real-time pace: the plugin's timeouts are wall-clock based,
    // and the peer on the other end of the server talks in real seconds too.
    for (int i = 0; i < 70; ++i) {           // 70 * 0.2 s = 14 s
        // dead-reckon east so the server sees us moving
        lon += (gsMs * 0.2) / (111320.0 * cos(lat * M_PI / 180.0));
        setPos(lat, lon, altM, 90.0f, gsMs);

        if (i == 20) harness::ptt(true);
        if (i == 25) harness::ptt(false);
        if (i == 30) harness::menu(2);        // "Send test message"

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

        if (i == 69 || (connected && sawTraffic && sawChat)) {
            printf("\n--- what the plugin window shows ---\n");
            for (const auto& l : lines) printf("  %s\n", l.c_str());
            if (connected && sawTraffic && sawChat) break;
        }
    }

    printf("\nconnected=%d traffic=%d chat=%d\n", connected, sawTraffic, sawChat);

    XPluginDisable();
    XPluginStop();

    if (!connected) { fprintf(stderr, "FAIL: never connected\n"); return 1; }
    if (!sawTraffic) { fprintf(stderr, "FAIL: never saw traffic\n"); return 1; }
    if (!sawChat)    { fprintf(stderr, "FAIL: never received a radio message\n"); return 1; }
    printf("harness OK\n");
    return 0;
}
