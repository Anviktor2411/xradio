// Tower mode (v0.6.0): control traffic from the tower cab inside X-Plane, with TowerTrainer as the authority.
//
// The plugin connects to TowerTrainer's tower link (server.py --tower-port, newline-separated JSON) as a
// remote controller of the trainer's session: it receives the state snapshots the trainer's own pop-out
// windows get, and sends back the same messages a remote controller sends (claim, tx, strip, ops, chat).
// In the sim it places the camera in the cab (apt.dat row 14, the trainer's OpenStreetMap tower, or the
// apron centre), freezes the user's aircraft so it cannot roll away, opens five text windows (mini radar,
// strips, Delivery list, frequency log with a command line, ATIS / operations) and -- when XRadio itself
// is not connected -- draws the trainer's traffic as CSL models. Everything works against server.py alone.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xr {
namespace tower {

struct Config {
    std::string host     = "127.0.0.1";   // TowerTrainer's PC
    std::string port     = "8781";        // its --tower-port
    std::string code     = "TOWER";       // the session code the trainer hosts for X-Plane
    std::string position = "TWR";         // the position claimed on entering
    std::string name     = "X-Plane";     // nickname in the trainer's roster
    bool        park     = true;          // freeze my own aircraft while in the tower, restore it on leaving
    bool        syncWeather = false;      // visibility, wind and time of day from the trainer (best effort)
    float       cabHeightM = 0.f;         // 0 = apt.dat row 14 / 30 m
    bool        draw3d   = true;          // the trainer's traffic as CSL models when XRadio is not connected
    bool        showWindows = true;
};

struct Hooks {
    std::function<void(const std::string&)> log;              // the plugin's log line
    std::function<bool()>                   xradioConnected;  // the traffic already comes through XRadio
    std::function<std::string()>            xplaneRoot;       // X-Plane folder, for apt.dat
    std::function<bool()>                   pttDown;          // for the TX indicator
};

struct Aircraft {
    int         id = 0;
    std::string cs, type, kind, state, freq, squawk, dest;
    double      lat = 0, lon = 0;
    float       x = 0, y = 0;             // the trainer's local metres (east, north)
    float       altFt = 0, hdg = 0, gsKt = 0;
    bool        onGround = true, veh = false, human = false;
    bool        handoffIn = false;        // a strip offered to my position, not yet accepted
    std::string handoffFrom;
    int         handoffAge = 0;
};

struct Runway {
    std::string name;
    float       width = 45.f;
    struct End { std::string name; float x = 0, y = 0; };
    std::vector<End> ends;
};

struct State {
    bool        linked = false;
    std::string linkStatus;               // for the windows and the main window line
    std::string myId, myPos;
    std::string icao, aptName, rwy, atis, metar, hostNick;
    double      lat0 = 0, lon0 = 0, kx = 1, ky = 1;
    bool        haveAirport = false;
    bool        haveTower = false;
    float       towerX = 0, towerY = 0;   // the trainer's OSM tower, local metres
    std::vector<Runway> runways;
    std::vector<std::pair<float, float>> holdLines;
    std::vector<Aircraft> ac;
    std::vector<std::string> logLines;    // my frequency, plain text, newest last
    std::vector<std::string> delLines;
    std::map<std::string, std::string> positionFreq;
    std::vector<std::pair<std::string, float>> alerts;   // text, time it arrived (plugin seconds)
    std::vector<std::pair<std::string, float>> live;     // relayed radio calls on my frequency, newest last
    bool        lvp = false;
    long long   utcMs = 0;
    int         selectedId = -1;
    float       lastStateAt = -1000.f;
};

// The camera as the tests see it.
struct CamView {
    bool   on = false;
    float  yaw = 0, pitch = 0, zoom = 1;
    double lat = 0, lon = 0, altM = 0;
    int    locked = -1;
};

// apt.dat row 14 (tower viewpoint) for one airport.
struct AptView {
    bool        found = false;
    double      lat = 0, lon = 0;
    float       heightM = 0;
    std::string name;
};
AptView parseAptTower(const std::string& text, const std::string& icao);
AptView findAptTower(const std::string& xplaneRoot, const std::string& icao);

void init(const Hooks& hooks);
void applyConfig(const Config& cfg);
void createWindows();                      // after the plugin's own windows (the harness numbers them)
void shutdown();
void tick(float dt, float elapsed);        // from the flight loop, every frame

bool active();
void toggle();
void enter();
void leave();
void groundView();                         // camera to the apron, 2 m up
void towerView();                          // back to the cab (after X-Plane took the view away)
void look(float dYawDeg, float dPitchDeg);
void zoom(float factor);                   // > 1 zooms in (binoculars)
void nextTarget();                         // lock the camera on the next aircraft
void setWindowsVisible(bool on);
bool windowsVisible();

std::string statusLine();
const State& state();
CamView camera();
const Config& config();

// Weather parsing (public for the tests): visibility in metres and wind from a METAR.
struct Wx { bool ok = false; float visM = 10000.f; float windDeg = 0.f, windKt = 0.f, gustKt = 0.f; };
Wx parseMetar(const std::string& metar);

}  // namespace tower
}  // namespace xr
