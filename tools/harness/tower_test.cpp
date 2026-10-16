// Tower mode (v0.6.0) without X-Plane: the JSON reader, the apt.dat tower viewpoint from a fixture, the METAR
// parser, and -- with XR_TOWER_PORT set to a running tools/fake_towertrainer.py -- the whole thing: with nobody
// at the port the link reports why and backs off (and Retry jumps the queue), then entering takes the camera to
// the trainer's tower and freezes the user's aircraft, the airport and traffic arrive, every action the windows
// have (accept, transmit, a strip to another bay, LVP, runway, stop bars, light gun) reaches the trainer as one
// message, and leaving restores the aircraft exactly. The windows themselves: tools/harness/tower_ui_test.
//   tools/fake_towertrainer.py --port 8781 --log received.txt &
//   XR_TOWER_PORT=8781 XR_TOWER_LOG=received.txt ./tower_test
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

#include "XPLMGraphics.h"
#include "XPLMPlugin.h"
#include "harness.h"
#include "../../plugin/src/json.h"
#include "../../plugin/src/tower.h"

PLUGIN_API int  XPluginStart(char*, char*, char*);
PLUGIN_API int  XPluginEnable(void);
PLUGIN_API void XPluginDisable(void);
PLUGIN_API void XPluginStop(void);

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}
static bool contains(const std::vector<std::string>& lines, const std::string& needle) { return harness::drawnContains(lines, needle); }
static std::string readAll(const std::string& path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main(int argc, char** argv) {
    // ---- JSON ----
    xr::json::Value v;
    std::string err;
    check(xr::json::parse("{\"type\":\"state\",\"n\":-12.5e1,\"ok\":true,\"none\":null,\"s\":\"a\\\"b\\u00e4\\ud83d\\ude00\",\"ac\":[{\"id\":1,\"pos\":[1.5,-2]},{\"id\":2}]}", &v, &err), ("json: parses a state-like object " + err).c_str());
    check(v.s("type") == "state" && v.n("n") == -125.0 && v.truthy("ok") && v.at("none") && v.at("none")->isNull(), "json: strings, numbers, booleans, null");
    check(v.s("s") == std::string("a\"b\xc3\xa4\xf0\x9f\x98\x80"), "json: escapes incl. \\u00e4 and a surrogate pair -> UTF-8");
    check(v.a("ac").size() == 2 && (int)v.a("ac")[0].n("id") == 1 && v.a("ac")[0].a("pos")[1].num == -2.0 && v.a("ac")[1].a("pos").size() == 0, "json: arrays and nested lookups with defaults");
    check(!xr::json::parse("{\"a\":1,}", &v, &err) && !xr::json::parse("[1,2", &v, &err) && !xr::json::parse("{} x", &v, &err), "json: malformed input is refused");
    check(xr::json::quote("he said \"hi\"\n") == "\"he said \\\"hi\\\"\\n\"", "json: quote() escapes");
    check(xr::json::stripHtml("<b>FIN321</b> Tallinn Tower, &amp; holding point <span class=\"x\">26</span>") == "FIN321 Tallinn Tower, & holding point 26", "json: stripHtml drops tags and decodes entities");

    // ---- apt.dat row 14 ----
    const char* fx = getenv("XR_APT_FIXTURE");
    const std::string fixture = readAll(fx ? fx : "tools/fixtures/apt_tower.dat");
    check(!fixture.empty(), "fixture tools/fixtures/apt_tower.dat readable (run from the repo root or set XR_APT_FIXTURE)");
    xr::tower::AptView a = xr::tower::parseAptTower(fixture, "EETN");
    check(a.found && std::fabs(a.lat - 59.4146) < 1e-4 && std::fabs(a.lon - 24.8412) < 1e-4 && a.heightM == 42.f && a.name == "Tallinn tower", "apt.dat: EETN tower viewpoint (row 14) with height and name");
    check(!xr::tower::parseAptTower(fixture, "EEPU").found, "apt.dat: an airport without row 14 reports none (and does not take the next airport's)");
    xr::tower::AptView s = xr::tower::parseAptTower(fixture, "EGSS");
    check(s.found && s.heightM == 36.f && s.name == "Stansted VCR", "apt.dat: a later airport is found");
    check(!xr::tower::parseAptTower(fixture, "ZZZZ").found, "apt.dat: unknown airport -> none");
    // findAptTower over a fake X-Plane folder layout
    const std::string root = harness::tempDir() + "/xp-tower";
    harness::ensureTempDir();
#ifdef _WIN32
    system(("mkdir \"" + root + "\\Global Scenery\\Global Airports\\Earth nav data\" >nul 2>&1").c_str());
    system(("mkdir \"" + root + "\\Custom Scenery\\MyEETN\\Earth nav data\" >nul 2>&1").c_str());
#else
    system(("mkdir -p '" + root + "/Global Scenery/Global Airports/Earth nav data' '" + root + "/Custom Scenery/MyEETN/Earth nav data'").c_str());
#endif
    { std::ofstream(root + "/Global Scenery/Global Airports/Earth nav data/apt.dat") << fixture; }
    { std::ofstream(root + "/Custom Scenery/MyEETN/Earth nav data/apt.dat") << "I\n1200\n\n1    131 0 0 EETN Custom Tallinn\n14  59.41500000  24.84000000 55 1 Custom cab\n99\n"; }
    { std::ofstream(root + "/Custom Scenery/scenery_packs.ini") << "I\n1000 Version\nSCENERY\n\nSCENERY_PACK Custom Scenery/MyEETN/\n"; }
    xr::tower::AptView f = xr::tower::findAptTower(root, "EETN");
    check(f.found && f.heightM == 55.f && f.name == "Custom cab", "apt.dat: a custom scenery airport wins over the global one");
    xr::tower::AptView f2 = xr::tower::findAptTower(root, "EGSS");
    check(f2.found && f2.heightM == 36.f, "apt.dat: the global apt.dat is searched when custom scenery has nothing");

    // ---- METAR ----
    xr::tower::Wx w = xr::tower::parseMetar("METAR EETN 150620Z 20006KT 9999 OVC007 09/07 Q1022");
    check(w.ok && w.windDeg == 200.f && w.windKt == 6.f && w.visM == 10000.f, "metar: wind and 9999 visibility");
    w = xr::tower::parseMetar("EGSS 151020Z 24015G28KT 0800 R22/P1500 FG BKN002 05/05 Q1001");
    check(w.ok && w.windDeg == 240.f && w.windKt == 15.f && w.gustKt == 28.f && w.visM == 800.f, "metar: gusts and 800 m visibility");
    w = xr::tower::parseMetar("KJFK 151051Z 31012KT 1 1/2SM BR OVC004 M01/M02 A2992");
    check(w.ok && std::fabs(w.visM - 1.5f * 1609.34f) < 2.f, "metar: statute miles with a whole and a fraction token (1 1/2SM)");

    // ---- the link against tools/fake_towertrainer.py ----
    const char* portEnv = getenv("XR_TOWER_PORT");
    if (!portEnv) {
        printf("SKIP  link test (set XR_TOWER_PORT to a running tools/fake_towertrainer.py)\n");
        printf(fails ? "%d FAILED\n" : "tower_test OK\n", fails);
        return fails ? 1 : 0;
    }
    harness::setEnv("XR_HARNESS", "1");
    auto writeCfg = [](const std::string& port) {
        std::ofstream cfg(harness::cfgPath());
        cfg << "callsign = ESXYZ\nhost = 127.0.0.1\nport = 49199\nautoconnect = no\n"
            << "towerhost = 127.0.0.1\ntowerport = " << port << "\ntowercode = TOWER\ntowerpos = TWR\ntowername = X-Plane\ntowerpark = yes\ntowerweather = yes\ntower3d = no\n";
    };
    writeCfg("1");     // nothing listens there: the trainer is "not running" until the config points at the fake
    harness::set("sim/flightmodel/position/local_x", 1234.5);
    harness::set("sim/flightmodel/position/local_y", 42.25);
    harness::set("sim/flightmodel/position/local_z", -987.125);
    harness::set("sim/flightmodel/position/psi", 123.5);
    harness::set("sim/flightmodel/position/theta", 1.25);
    harness::set("sim/flightmodel/position/phi", -0.5);
    harness::set("sim/flightmodel/position/local_vx", 3.0);
    harness::set("sim/flightmodel/position/local_vy", 0.1);
    harness::set("sim/flightmodel/position/local_vz", -4.0);
    harness::setArray("sim/flightmodel/position/q", {0.9f, 0.1f, 0.2f, 0.3f});
    harness::setArray("sim/operation/override/override_planepath", {0.f});
    harness::setTerrain(40.0);
    char n[256], sg[256], d[256];
    XPluginStart(n, sg, d);
    XPluginEnable();
    check(!xr::tower::active() && xr::tower::state().phase == xr::tower::Phase::Off, "tower mode starts off");
    xr::tower::toggle();
    check(xr::tower::active(), "toggle enters tower mode");
    auto tickUntil = [](float seconds, const std::function<bool()>& done) {
        for (int i = 0; i < (int)(seconds * 10) && !done(); ++i) { harness::tick(0.1f); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        return done();
    };
    // ---- TowerTrainer not running: the banner's facts, the back-off, Retry jumping it
    const xr::tower::State& st = xr::tower::state();
    const bool down = tickUntil(5.f, [&] { return st.phase == xr::tower::Phase::Unreachable && !st.linkError.empty(); });
    check(down && st.endpoint == "127.0.0.1:1", ("nobody at the port: Unreachable, with the address and why (" + st.linkError + ")").c_str());
    check(tickUntil(12.f, [&] { return st.retryInS > 2.5f; }), "the next try backs off (1 s, 2 s, 4 s ...)");
    const int attemptsBefore = st.linkAttempts;
    xr::tower::retry();
    check(tickUntil(0.6f, [&] { return st.linkAttempts > attemptsBefore; }), "Retry tries at once instead of waiting out the back-off");
    float nextWait = -1.f;     // the first wait after that attempt (a refused connect takes a while on Windows)
    tickUntil(5.f, [&] { if (st.linkAttempts > attemptsBefore && st.retryInS > 0.f && nextWait < 0.f) nextWait = st.retryInS; return nextWait >= 0.f; });
    check(nextWait > 0.f && nextWait <= 1.05f, ("...and starts the back-off over at one second (" + std::to_string(nextWait) + " s)").c_str());
    check(xr::tower::windowsVisible(), "the windows are wanted (the user interface opens them once linked)");
    // ---- the fake trainer
    {
        xr::tower::Config c = xr::tower::config();
        c.port = portEnv;
        xr::tower::applyConfig(c);
    }
    const bool linked = tickUntil(10.f, [&] { return st.linked && st.haveAirport && !st.ac.empty() && st.myPos == "TWR" && st.phase == xr::tower::Phase::Linked; });
    check(linked, ("linked to the fake trainer, airport and traffic received, TWR claimed (" + st.linkStatus + ")").c_str());
    check(st.icao == "EETN" && st.runways.size() == 1 && st.runways[0].ends[0].name == "08" && st.haveTower, "airport line parsed: EETN, runway 08/26, OSM tower");
    check(st.stands.size() == 11 && st.taxiNodes.size() == 16 && st.taxiEdges.size() == 14 && st.pavement.size() == 2 && st.holdLines.size() == 5,
          "and what the ground radar draws: stands, the taxi network, the pavement, the holding points");
    check(st.delivery.size() == 3 && st.delivery[1].cs == "BTI4KT" && st.delivery[1].clearance.rfind("Air Baltic four kilo tango cleared to Riga", 0) == 0 && st.delivery[2].cleared,
          "the trainer's Delivery list: requests with its clearance text, a cleared one marked");
    check(st.haveStopBars && st.stopBars && st.positions.count("TWR") && st.positions.at("TWR").freq == "135.905", "stop bars lit, the positions' frequencies");
    check(st.log.size() == 9 && st.log[3].from == "FIN321" && st.log[3].cls == "pilot dep" && st.log[4].from == "EETN_TWR", "the TWR log in entries: sender, class, text");
    const xr::tower::Aircraft* fin = nullptr; const xr::tower::Aircraft* sas = nullptr; const xr::tower::Aircraft* lot = nullptr; const xr::tower::Aircraft* dlh = nullptr;
    for (const auto& ac : st.ac) { if (ac.cs == "FIN321") fin = &ac; if (ac.cs == "SAS123") sas = &ac; if (ac.cs == "LOT789") lot = &ac; if (ac.cs == "DLH5KM") dlh = &ac; }
    check(fin && sas && lot && dlh && fin->freq == "TWR" && std::fabs(fin->lat - (59.413333 + (-40.0) / 110540.0)) < 1e-6 && std::fabs(fin->lon - (24.8325 + 1900.0 / (cos(59.413333 * 3.14159265 / 180) * 111320.0))) < 1e-6,
          "aircraft positions converted from the trainer's metres to lat/lon");
    check(sas && sas->handoffIn && sas->handoffFrom == "GND" && fin && fin->sid == "ULGOT1G" && fin->cfl == 4000 && fin->tel == "Finnair", "an incoming handoff; SID, cleared level and telephony on the strip");
    xr::tower::CamView cam = xr::tower::camera();
    const double towerLat = 59.413333 + (-250.0) / 110540.0, towerLon = 24.8325 + 600.0 / (cos(59.413333 * 3.14159265 / 180) * 111320.0);
    check(harness::cameraControlled() && cam.on && std::fabs(cam.lat - towerLat) < 1e-6 && std::fabs(cam.lon - towerLon) < 1e-6 && std::fabs(cam.altM - 70.0) < 0.01,
          "camera placed at the trainer's tower, 30 m above the probed terrain (no apt.dat here)");
    XPLMCameraPosition_t cp = harness::camera();
    double clat, clon, calt;
    XPLMLocalToWorld(cp.x, cp.y, cp.z, &clat, &clon, &calt);
    check(std::fabs(clat - towerLat) < 1e-6 && std::fabs(clon - towerLon) < 1e-6 && std::fabs(calt - 70.0) < 0.01 && cp.zoom == 1.f, "the camera callback hands X-Plane the cab position");
    check(harness::getArray("sim/operation/override/override_planepath").size() >= 1 && harness::getArray("sim/operation/override/override_planepath")[0] == 1.f, "the user's aircraft is frozen (override_planepath)");
    check(harness::get("sim/flightmodel/position/local_x") == 1234.5 && harness::get("sim/flightmodel/position/local_vx") == 0.0, "the aircraft stays where it was, with no velocity");
    // the weather sync wrote visibility and wind from the METAR
    check(std::fabs(harness::get("sim/weather/region/visibility_reported_sm") - 10000.f / 1609.34f) < 0.01 && harness::getArray("sim/weather/region/wind_direction_degt").size() >= 1 && harness::getArray("sim/weather/region/wind_direction_degt")[0] == 200.f, "visibility and wind from the trainer's METAR");
    // ---- what the windows do, each one message to the trainer
    const std::string logPath = getenv("XR_TOWER_LOG") ? getenv("XR_TOWER_LOG") : "received.txt";
    auto received = [&](const std::string& needle) { return tickUntil(3.f, [&] { return readAll(logPath).find(needle) != std::string::npos; }); };
    const int sasId = sas ? sas->id : -1, finId = fin ? fin->id : -1, lotId = lot ? lot->id : -1, dlhId = dlh ? dlh->id : -1;
    xr::tower::acceptHandoff(sasId);
    check(received("{\"type\":\"strip\",\"id\":2,\"accept\":true}"), "Accept on the incoming strip sends the acceptance to the trainer");
    xr::tower::select(finId);
    check(st.selectedId == finId, "selecting an aircraft");
    xr::tower::transmit("FIN321 runway 26 cleared for takeoff");
    check(received("\"type\":\"tx\",\"pos\":\"TWR\",\"text\":\"FIN321 runway 26 cleared for takeoff\""), "a typed instruction goes to the trainer as my transmission on TWR");
    bool mine = false, roger = false;
    tickUntil(3.f, [&] { for (const auto& c : st.live) { mine |= c.mine && c.text == "FIN321 runway 26 cleared for takeoff"; roger |= c.from == "FIN321" && c.text.rfind("Roger", 0) == 0; } return mine && roger; });
    check(mine && roger, "the live feed shows my call and the pilot's answer until the trainer's log carries them");
    xr::tower::moveStrip(lotId, "holding");
    check(received("{\"type\":\"strip\",\"id\":9,\"bay\":\"holding\"}"), "dropping a strip on another bay asks the trainer to move it");
    check(tickUntil(3.f, [&] { for (const auto& a : st.ac) if (a.cs == "LOT789") return a.stripBay == "holding" && a.stripState == a.state; return false; }),
          "the trainer's next state has LOT789 in the Holding bay");
    xr::tower::setLvp(true);
    check(received("\"type\":\"ops\",\"what\":\"lvp\",\"value\":true"), "the LVP switch sends an ops request");
    xr::tower::setRunway("08");
    check(received("\"type\":\"ops\",\"what\":\"runway\",\"value\":\"08\""), "picking a runway sends it");
    xr::tower::setStopBars(false);
    check(received("\"type\":\"ops\",\"what\":\"stopbars\",\"value\":false"), "the stop bars switch sends it");
    xr::tower::lightGun(finId, "green steady");
    check(received("\"text\":\"FIN321 light green steady\""), "the light gun is an instruction to the selected aircraft");
    xr::tower::lightGun(9999, "red steady");
    xr::tower::moveStrip(9999, "holding");
    xr::tower::acceptHandoff(9999);
    harness::tick(0.1f); std::this_thread::sleep_for(std::chrono::milliseconds(300)); harness::tick(0.1f);
    const std::string got = readAll(logPath);
    check(got.find("9999") == std::string::npos, "nothing is sent about an aircraft the trainer does not have");
    // ---- the camera
    xr::tower::look(30.f, 5.f);
    check(xr::tower::camera().yaw != cam.yaw && xr::tower::camera().pitch == cam.pitch + 5.f, "look() turns the camera");
    xr::tower::zoom(2.f);
    check(xr::tower::camera().zoom == 2.f, "zoom() for the binoculars");
    xr::tower::follow(dlhId);
    harness::tick(0.1f);
    check(xr::tower::camera().locked == dlhId && st.followId == dlhId, "follow() locks the camera on an aircraft (the radar shows it followed)");
    xr::tower::follow(-1);
    check(xr::tower::camera().locked == -1, "follow(-1) lets go");
    const float yawBefore = xr::tower::camera().yaw;
    xr::tower::lookAt(finId);
    check(xr::tower::camera().yaw != yawBefore && xr::tower::camera().locked == -1, "lookAt() turns towards an aircraft once, without following it");
    xr::tower::nextTarget();
    harness::tick(0.1f);
    check(xr::tower::camera().locked >= 0, "nextTarget() follows an aircraft");
    check(!xr::tower::statusLine().empty() && xr::tower::statusLine().find("EETN") != std::string::npos, ("status line: " + xr::tower::statusLine()).c_str());
    // ---- leave: everything back
    xr::tower::toggle();
    check(!xr::tower::active() && !harness::cameraControlled() && st.phase == xr::tower::Phase::Off, "leaving releases the camera");
    check(harness::getArray("sim/operation/override/override_planepath")[0] == 0.f && harness::get("sim/flightmodel/position/local_x") == 1234.5 && harness::get("sim/flightmodel/position/local_y") == 42.25 &&
          harness::get("sim/flightmodel/position/local_z") == -987.125 && harness::get("sim/flightmodel/position/psi") == 123.5 && harness::get("sim/flightmodel/position/local_vx") == 3.0 && harness::get("sim/flightmodel/position/local_vz") == -4.0 &&
          harness::getArray("sim/flightmodel/position/q")[1] == 0.1f, "the aircraft is restored exactly: position, attitude, quaternion, velocities, override off");
    check(!xr::tower::windowsVisible(), "the tower windows are not wanted any more");
    XPluginDisable();
    XPluginStop();
    printf(fails ? "%d FAILED\n" : "tower_test OK\n", fails);
    return fails ? 1 : 0;
}
