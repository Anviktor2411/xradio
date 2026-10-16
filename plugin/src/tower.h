// Tower mode (v0.6.0): control traffic from the tower cab inside X-Plane, with TowerTrainer as the authority.
//
// The plugin connects to TowerTrainer's tower link (server.py --tower-port, newline-separated JSON) as a
// remote controller of the trainer's session: it receives the state snapshots the trainer's own pop-out
// windows get, and sends back the same messages a remote controller sends (claim, tx, strip, ops, chat).
// In the sim it places the camera in the cab (apt.dat row 14, the trainer's OpenStreetMap tower, or the
// apron centre), freezes the user's aircraft so it cannot roll away, and -- when XRadio itself is not
// connected -- draws the trainer's traffic as CSL models. Everything works against server.py alone.
//
// This is the logic only. The windows -- ground radar, strips, Delivery, frequency, ATIS / operations and
// the connection banner -- are Dear ImGui in ui2/tower_ui.cpp: they draw State and act through the
// functions below, so the test harness drives exactly what a click in a window does.
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
    std::string cs, tel, num, type, wtc, kind, state, freq, squawk, assignedSq;
    std::string other, otherName;         // the far end: a departure's destination, an arrival's origin
    std::string stand, rwy, sid, vtype;
    int         cfl = 0;                  // feet; 0 = none assigned
    double      lat = 0, lon = 0;
    float       x = 0, y = 0;             // the trainer's local metres (east, north)
    float       altFt = 0, hdg = 0, gsKt = 0;
    bool        onGround = true, veh = false, human = false, ga = false;
    bool        quiet = false, dormant = false;   // a service vehicle; an aircraft parked for the night
    bool        handoffIn = false;        // a strip offered to my position, not yet accepted
    std::string handoffFrom, handoffTo;
    int         handoffAge = 0;
    std::string stripBay, stripState;     // where a controller put the strip, and the state it was in then
    long long   callingUntilMs = 0;       // the trainer's clock (State::utcMs): it is calling until then
    int         waitingS = -1;            // seconds since it asked (a clearance), -1 when not waiting
    std::vector<std::string> warn;        // the trainer's label warnings: EMG, RCF, SQK 4521 ...
};

struct Runway {
    std::string name;
    float       width = 45.f;
    struct End { std::string name; float x = 0, y = 0; };
    std::vector<End> ends;
};

struct Pt { float x = 0, y = 0; };
struct Stand { std::string name; float x = 0, y = 0, hdg = 0; };
struct TaxiEdge { int a = 0, b = 0; std::string name; float widthM = 15.f; };
using Ring = std::vector<Pt>;
using Polygon = std::vector<Ring>;        // the outer ring, then its holes

// One line of my position's frequency log, as the trainer wrote it.
struct LogEntry {
    std::string cls;                      // "atc", "ai", "pilot dep", "pilot human", "sys", "blk", "err"
    std::string time, from, text;
    bool        guard = false;            // heard on 121.5
};

// A relayed radio call on my frequency, shown live until the trainer's log carries it.
struct LiveCall { std::string from, text; float at = 0; bool mine = false, atc = false; };

// One line of the trainer's Delivery list: a clearance request with what the trainer suggests.
struct DelRequest {
    int         id = 0;
    std::string cs, type, wtc, dest, destName, stand, sid, cfl, squawk, clearance, note;
    int         waitingS = -1;
    bool        cleared = false, live = false;
};

struct Position { std::string name, freq, cs; };

// Where the link stands, for the banner (one place, instead of raw text in every window).
enum class Phase { Off, Connecting, Unreachable, NoSession, Linked, HostAway };

struct State {
    bool        linked = false;
    std::string linkStatus;               // the line in the main window and the log
    Phase       phase = Phase::Off;
    std::string endpoint;                 // "127.0.0.1:8781"
    std::string linkError;                // why the last attempt failed
    float       retryInS = 0.f;           // until the next attempt, while unreachable
    int         linkAttempts = 0;         // connection attempts since entering
    std::string myId, myPos;
    std::string icao, aptName, rwy, atis, metar, hostNick;
    double      lat0 = 0, lon0 = 0, kx = 1, ky = 1;
    bool        haveAirport = false;
    bool        haveTower = false;
    float       towerX = 0, towerY = 0;   // the trainer's OSM tower, local metres
    std::vector<Runway> runways;
    std::vector<Pt> holdLines;
    std::vector<Stand> stands;
    std::vector<Pt> taxiNodes;
    std::vector<TaxiEdge> taxiEdges;
    std::vector<Polygon> pavement;
    std::vector<Aircraft> ac;
    std::vector<LogEntry> log;            // my frequency, newest last
    std::vector<DelRequest> delivery;     // from the trainer, or put together here for an older one
    std::map<std::string, Position> positions;
    std::vector<int> closedRunways;       // indexes into runways
    std::vector<std::pair<std::string, float>> alerts;   // text, time it arrived (plugin seconds)
    std::vector<LiveCall> live;           // newest last
    bool        split = false;
    bool        lvp = false;
    bool        stopBars = false, haveStopBars = false;
    long long   utcMs = 0;
    int         selectedId = -1;
    int         followId = -1;            // the camera follows this aircraft
    bool        pttDown = false;
    float       now = 0.f;                // plugin seconds, for ages and blinking
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

// One line of the trainer's log ("<span class="t">0:04</span><b>FIN321</b> ready") taken apart.
LogEntry parseLogEntry(const std::string& cls, const std::string& html);

void init(const Hooks& hooks);
void applyConfig(const Config& cfg);
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
void follow(int id);                       // the camera follows this aircraft; -1 lets go
void lookAt(int id);                       // turn the camera towards it once
void setWindowsVisible(bool on);
bool windowsVisible();

// What the windows do. Each is one message to the trainer, exactly what its own window would send.
void select(int id);                       // -1: nothing selected
void transmit(const std::string& text);    // a typed instruction, as my transmission on my position
void acceptHandoff(int id);
void moveStrip(int id, const std::string& bay);
void setLvp(bool on);
void setRunway(const std::string& end);    // the runway in use, by end ("26")
void setStopBars(bool on);
void lightGun(int id, const std::string& signal);   // "green steady", "red flashing" ... for one aircraft
void retry();                              // try the link again now, instead of after the back-off

std::string statusLine();
const State& state();
CamView camera();
const Config& config();

// Weather parsing (public for the tests): visibility in metres and wind from a METAR.
struct Wx { bool ok = false; float visM = 10000.f; float windDeg = 0.f, windKt = 0.f, gustKt = 0.f; };
Wx parseMetar(const std::string& metar);

}  // namespace tower
}  // namespace xr
