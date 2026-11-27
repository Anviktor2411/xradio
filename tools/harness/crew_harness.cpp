// 0.7: the shared cockpit end to end -- the real plugin, a real server, fake pilots.
//
//   ./crew_harness <host> <port>
//
// Expects, on that server:
//   PIC001  tools/fake_client.py --crew-open --parrot --intercom-parrot --com1 124300 --com2 121500
//   COP001  tools/fake_client.py --crew-join CREWHN --crew-code ABC --crew-tune com2=118.500
//
// Part one: the plugin (CREWHN) joins PIC001's cockpit as copilot -- aboard, its own radio panel follows the
// aircraft's, a knob it turns goes to the pilot's sim, the intercom reaches the pilot and comes back, and its
// radio transmission goes out as the aircraft (PIC001 hears it in the cockpit and says it again on the air).
// Part two: the plugin opens its own cockpit with a code; COP001 joins it and retunes COM2 in the plugin's sim.
#include "harness.h"
#include "protocol.h"
#include "voice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

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
    fflush(stdout);
    if (!ok) ++failures;
}

static const char* kCom1 = "sim/cockpit2/radios/actuators/com1_frequency_hz_833";
static const char* kCom2 = "sim/cockpit2/radios/actuators/com2_frequency_hz_833";
static const char* kCom1Stby = "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833";

// Real seconds of flight loop: the server and the fake pilots talk in wall-clock time.
static void fly(double seconds, const std::function<void()>& each = nullptr) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (std::chrono::steady_clock::now() < until) {
        harness::tick(0.05f);
        if (each) each();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// Fly until the text window says `needle`, up to `seconds`.
static bool waitFor(const std::string& needle, double seconds, std::vector<std::string>* last = nullptr) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    std::vector<std::string> lines;
    while (std::chrono::steady_clock::now() < until) {
        harness::tick(0.05f);
        lines = harness::draw();
        if (harness::drawnContains(lines, needle)) { if (last) *last = lines; return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (last) *last = lines;
    return false;
}

static bool waitRef(const char* ref, double want, double seconds) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (std::chrono::steady_clock::now() < until) {
        harness::tick(0.05f);
        harness::draw();
        if (std::fabs(harness::get(ref) - want) < 0.5) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

static void writeCfg(const char* host, const char* port, const std::string& crew) {
    FILE* f = fopen(harness::cfgPath().c_str(), "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", harness::cfgPath().c_str()); exit(1); }
    fprintf(f, "host = %s\nport = %s\ncallsign = CREWHN\nactype = C172\nautoconnect = yes\ncheckupdates = no\n%s",
            host, port, crew.c_str());
    fclose(f);
}

static void dump(const std::vector<std::string>& lines) {
    for (const auto& l : lines) fprintf(stderr, "     | %s\n", l.c_str());
}

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const char* port = argc > 2 ? argv[2] : "49100";
    harness::g_verbose = (getenv("XR_VERBOSE") != nullptr);
    if (!harness::ensureTempDir()) return 1;
    writeCfg(host, port, "crewmode = join\ncrewjoin = PIC001\ncrewrole = copilot\ncrewsync = yes\n");

    // Near PIC001 (57.86, 27.03), powered up, our own radios on other frequencies to begin with.
    harness::set("sim/flightmodel/position/latitude", 57.861);
    harness::set("sim/flightmodel/position/longitude", 27.031);
    harness::set("sim/flightmodel/position/elevation", 914.0);
    harness::set("sim/flightmodel/position/psi", 90.0);
    harness::set("sim/flightmodel/position/groundspeed", 60.0);
    harness::set(kCom1, 119100);
    harness::set(kCom2, 122000);
    harness::set(kCom1Stby, 118000);
    harness::set("sim/cockpit2/radios/actuators/audio_selection_com1", 1);
    harness::set("sim/cockpit2/radios/actuators/audio_com_selection", 6);
    harness::set("sim/cockpit2/switches/avionics_power_on", 1);
    harness::set("sim/cockpit2/radios/actuators/com1_power", 1);
    harness::set("sim/cockpit2/radios/actuators/com2_power", 1);
    harness::set("sim/cockpit2/electrical/bus_volts", 24.0);
    harness::set("sim/cockpit2/radios/actuators/transponder_mode", xr::XPDR_ALT);

    char name[256], sig[256], desc[256];
    if (!XPluginStart(name, sig, desc) || !XPluginEnable()) { fprintf(stderr, "plugin did not start\n"); return 1; }

    printf("\npart one: the plugin joins PIC001's cockpit as copilot\n");
    std::vector<std::string> lines;
    check("connected", waitFor("connected to", 15.0, &lines));
    const bool aboard = waitFor("Aboard PIC001 as copilot", 20.0, &lines);
    check("aboard PIC001 as copilot, and the window says so", aboard);
    if (!aboard) dump(lines);
    check("its radio panel follows the aircraft's: COM1", waitRef(kCom1, 124300, 8.0), std::to_string(harness::get(kCom1)));
    check("COM2 and the standby too", std::fabs(harness::get(kCom2) - 121500) < 0.5 && std::fabs(harness::get(kCom1Stby) - 119700) < 0.5,
          std::to_string(harness::get(kCom2)) + " " + std::to_string(harness::get(kCom1Stby)));
    // A copilot turns COM1 in their own cockpit: the pilot's sim is asked, retunes, and its panel comes back.
    harness::set(kCom1, 127905);
    fly(4.0);
    check("a knob the copilot turns is the aircraft's: it stays where they put it", std::fabs(harness::get(kCom1) - 127905) < 0.5,
          std::to_string(harness::get(kCom1)));

    // The intercom: what we say reaches PIC001, who says it straight back on the intercom.
    const xr::voice::Stats before = xr::voice::stats();
    bool heardIc = false;
    harness::command("xradio/intercom", 0 /* xplm_CommandBegin */);
    fly(2.0, [&] { if (!xr::voice::activeIntercomSpeakers().empty()) heardIc = true; });
    harness::command("xradio/intercom", 2 /* xplm_CommandEnd */);
    fly(1.5, [&] { if (!xr::voice::activeIntercomSpeakers().empty()) heardIc = true; });
    const xr::voice::Stats after = xr::voice::stats();
    check("the intercom goes to the pilot and comes back from them", heardIc && after.framesReceived > before.framesReceived + 20,
          std::to_string(after.framesReceived - before.framesReceived) + " frames back");
    // The radio: we transmit as the aircraft; PIC001 hears it in the cockpit (from us) and, once we let go
    // of the one transmitter, says it again on the air -- which we hear in the cockpit, from the pilot.
    bool heardPilot = false;
    harness::ptt(true);
    fly(1.5);
    harness::ptt(false);
    fly(4.0, [&] { if (!xr::voice::activeSpeakers().empty()) heardPilot = true; });
    check("a transmission goes out on the aircraft's radio, and the pilot's comes back on it", heardPilot);
    // A traffic row starts with the callsign padded to eight: "COP001   C172 ...". COP001 is a pilot nearby
    // (not aboard anything yet); PIC001 is the aircraft we are sitting in.
    lines = harness::draw();
    bool listedPic = false, listedCop = false;
    for (const auto& l : lines) {
        if (l.rfind("PIC001  ", 0) == 0) listedPic = true;
        if (l.rfind("COP001  ", 0) == 0) listedCop = true;
    }
    check("COP001, nearby, is in our traffic list", listedCop);
    check("the aircraft we sit in is not", !listedPic);
    if (!listedCop || listedPic) dump(lines);

    printf("\npart two: the plugin opens its own cockpit, COP001 joins and retunes COM2\n");
    writeCfg(host, port, "crewmode = pilot\ncrewcode = ABC\n");
    harness::menu(2);                           // Plugins > XRadio > Reconnect: the file again, a new session
    harness::set(kCom2, 121500);
    const bool open = waitFor("Crew aboard: CREWHN (pilot), COP001 (copilot)", 25.0, &lines);
    check("COP001 is aboard with the code", open);
    if (!open) dump(lines);
    check("and turned COM2 in our sim, to 118.500", waitRef(kCom2, 118500, 10.0), std::to_string(harness::get(kCom2)));

    XPluginDisable();
    XPluginStop();
    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("crew harness OK\n");
    return 0;
}
