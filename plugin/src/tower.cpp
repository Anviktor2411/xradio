#include "tower.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "XPLMCamera.h"
#include "XPLMDataAccess.h"
#include "XPLMGraphics.h"
#include "XPLMScenery.h"

#include "json.h"
#include "tcp.h"
#include "tower_parse.h"
#include "xpmp_bridge.h"

namespace xr {
namespace tower {

namespace {

constexpr float kDegToRad = 3.14159265f / 180.f;
constexpr uint32_t kSidBase = 0x7E000000u;        // CSL ids of the trainer's traffic, away from XRadio's

long long steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ the link thread
class Link {
public:
    void start(const Config& cfg) {
        stop();
        cfg_ = cfg;
        run_ = true;
        kick_ = false;
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        run_ = false;
        sock_.close();
        if (th_.joinable()) th_.join();
        connected_ = false;
        unreachable_ = false;
    }
    void kick() { kick_ = true; }                  // the next attempt now, not after the back-off
    bool connected() const { return connected_; }
    bool unreachable() const { return unreachable_; }
    int attempts() const { return attempts_; }
    float retryIn() const { const long long d = nextTryMs_ - steadyMs(); return d > 0 ? d / 1000.f : 0.f; }
    void send(const std::string& line) {
        std::lock_guard<std::mutex> lk(mx_);
        if (outbox_.size() < 200) outbox_.push_back(line);
    }
    std::vector<std::string> drain() {
        std::lock_guard<std::mutex> lk(mx_);
        std::vector<std::string> out(inbox_.begin(), inbox_.end());
        inbox_.clear();
        return out;
    }
    std::string status() { std::lock_guard<std::mutex> lk(mx_); return status_; }
    std::string error() { std::lock_guard<std::mutex> lk(mx_); return err_; }

private:
    void setStatus(const std::string& s) { std::lock_guard<std::mutex> lk(mx_); status_ = s; }
    // Sleep until the next attempt, or until retry() asks for it now. Returns true when it was asked.
    bool wait(int ms) {
        nextTryMs_ = steadyMs() + ms;
        while (run_ && !kick_ && steadyMs() < nextTryMs_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        nextTryMs_ = 0;
        const bool kicked = kick_;
        kick_ = false;
        return kicked;
    }
    void loop() {
        int backoffMs = 1000;
        while (run_) {
            std::string err;
            ++attempts_;
            setStatus("connecting to " + cfg_.host + ":" + cfg_.port);
            if (!sock_.connect(cfg_.host, (uint16_t)atoi(cfg_.port.c_str()), 3000, &err)) {
                { std::lock_guard<std::mutex> lk(mx_); err_ = err; }
                unreachable_ = true;
                setStatus("TowerTrainer not reachable at " + cfg_.host + ":" + cfg_.port + " (" + err + ") -- is server.py running?");
                if (wait(backoffMs)) backoffMs = 1000; else backoffMs = std::min(10000, backoffMs * 2);
                continue;
            }
            backoffMs = 1000;
            unreachable_ = false;
            { std::lock_guard<std::mutex> lk(mx_); err_.clear(); }
            sock_.sendLine("{\"type\":\"hello\",\"code\":" + json::quote(cfg_.code) + ",\"name\":" + json::quote(cfg_.name) +
                           ",\"pos\":" + json::quote(cfg_.position) + ",\"version\":\"0.6.0\"}");
            connected_ = true;
            setStatus("linked");
            std::string buf;
            char chunk[65536];
            while (run_) {
                {   // pending lines from the main thread
                    std::vector<std::string> out;
                    { std::lock_guard<std::mutex> lk(mx_); out.assign(outbox_.begin(), outbox_.end()); outbox_.clear(); }
                    bool ok = true;
                    for (const auto& l : out) if (!sock_.sendLine(l)) { ok = false; break; }
                    if (!ok) break;
                }
                const int n = sock_.recv(chunk, sizeof chunk, 100);
                if (n < 0) break;
                if (n == 0) continue;
                buf.append(chunk, (size_t)n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string line = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    std::lock_guard<std::mutex> lk(mx_);
                    if (inbox_.size() > 400) inbox_.pop_front();
                    inbox_.push_back(std::move(line));
                }
                if (buf.size() > 8 * 1024 * 1024) buf.clear();
            }
            connected_ = false;
            sock_.close();
            if (run_) setStatus("link lost -- reconnecting");
            wait(2000);
        }
    }

    Config                  cfg_;
    TcpSocket               sock_;
    std::thread             th_;
    std::atomic<bool>       run_{false};
    std::atomic<bool>       connected_{false};
    std::atomic<bool>       unreachable_{false};
    std::atomic<bool>       kick_{false};
    std::atomic<long long>  nextTryMs_{0};
    std::atomic<int>        attempts_{0};
    std::mutex              mx_;
    std::deque<std::string> inbox_, outbox_;
    std::string             status_, err_;
};

// ------------------------------------------------------------------ everything else
struct Parked {
    bool   on = false;
    double x = 0, y = 0, z = 0;
    float  psi = 0, theta = 0, phi = 0;
    float  vx = 0, vy = 0, vz = 0;
    float  q[4] = {1, 0, 0, 0};
};

struct Refs {
    XPLMDataRef localX = nullptr, localY = nullptr, localZ = nullptr, psi = nullptr, theta = nullptr, phi = nullptr;
    XPLMDataRef vx = nullptr, vy = nullptr, vz = nullptr, q = nullptr, overridePath = nullptr;
    XPLMDataRef zulu = nullptr, visSm = nullptr, windDir = nullptr, windSpd = nullptr;
};

struct Sample { float x = 0, y = 0, altFt = 0, hdg = 0; float t = 0; };
struct Track { Sample a, b; bool have = false; };

struct G {
    Hooks   hooks;
    Config  cfg;
    State   st;
    Link    link;
    Refs    refs;
    Parked  parked;
    bool    active = false;
    bool    windowsOn = false;
    bool    sessionMissing = false;    // the trainer answered "no such session": sticky until a welcome
    bool    hostAway = false;
    float   elapsed = 0.f;
    float   lastPing = 0.f;
    float   lastWx = -1000.f;
    // camera
    bool    camOn = false;
    float   yaw = 0.f, pitch = -3.f, zoomF = 1.f;
    double  cabLat = 0, cabLon = 0, cabAltM = 0, cabHeightM = 30.0;
    bool    cabFromApt = false;
    bool    groundView = false;
    int     locked = -1;
    // apt.dat scan
    std::thread aptThread;
    std::mutex  aptMx;
    AptView     aptView;
    std::string aptScanned;
    bool        aptPending = false;
    // 3D traffic
    std::map<int, Track> tracks;
    std::map<int, uint32_t> drawn;
    bool        drewAny = false;
};
G& g() { static G inst; return inst; }

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g().hooks.log) g().hooks.log(std::string("tower: ") + buf);
}

inline double latOf(float y) { return g().st.lat0 + y / g().st.ky; }
inline double lonOf(float x) { return g().st.lon0 + x / g().st.kx; }

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// ------------------------------------------------------------------ messages to and from the trainer
void sendLine(const std::string& l) { g().link.send(l); }
void sendClaim() {
    if (!g().cfg.position.empty()) sendLine("{\"type\":\"claim\",\"pos\":" + json::quote(g().cfg.position) + "}");
}
std::string myPosition() { return !g().st.myPos.empty() ? g().st.myPos : g().cfg.position; }
const Aircraft* findAc(int id) {
    for (const auto& a : g().st.ac) if (a.id == id) return &a;
    return nullptr;
}

void onAirport(const json::Value& m) {
    State& s = g().st;
    applyAirport(s, m);
    logf("airport %s (%s) datum %.5f %.5f, %d runways, %d stands, %d taxiway segments, %d pavement areas, tower %s", s.icao.c_str(), s.aptName.c_str(), s.lat0, s.lon0,
         (int)s.runways.size(), (int)s.stands.size(), (int)s.taxiEdges.size(), (int)s.pavement.size(), s.haveTower ? "from OSM" : "unknown");
}

void onState(const json::Value& m) {
    State& s = g().st;
    applyState(s, m, g().cfg.position);
    s.lastStateAt = g().elapsed;
    // samples for the 3D traffic
    for (const auto& a : s.ac) {
        if (a.veh) continue;
        Track& t = g().tracks[a.id];
        Sample smp; smp.x = a.x; smp.y = a.y; smp.altFt = a.altFt; smp.hdg = a.hdg; smp.t = g().elapsed;
        if (!t.have) { t.a = t.b = smp; t.have = true; } else { t.a = t.b; t.b = smp; }
    }
}

void handleLine(const std::string& line) {
    json::Value m;
    std::string err;
    if (!json::parse(line, &m, &err) || !m.isObject()) return;
    const std::string type = m.s("type");
    State& s = g().st;
    if (type == "welcome") {
        s.myId = m.s("id");
        g().sessionMissing = false;
        g().hostAway = false;
        s.linkStatus = "in session " + m.s("name") + (m.truthy("host") || !m.at("host") ? "" : " (host away)");
        sendClaim();
    } else if (type == "airport") onAirport(m);
    else if (type == "state") onState(m);
    else if (type == "claims") {
        const json::Value* mine = m.o("claims").at(s.myId.c_str());
        const std::string before = s.myPos;
        s.myPos = mine && mine->isString() ? mine->str : "";
        if (s.myPos != before) logf("working %s", s.myPos.empty() ? "no position" : s.myPos.c_str());
    } else if (type == "alert" || type == "note") {
        s.alerts.emplace_back(m.s(type == "alert" ? "msg" : "text"), g().elapsed);
        if (s.alerts.size() > 6) s.alerts.erase(s.alerts.begin());
    } else if (type == "error" || type == "kicked") {
        const std::string e = m.s(type == "error" ? "error" : "reason");
        if (e.find("no such session") != std::string::npos) {
            g().sessionMissing = true;
            s.linkStatus = "no session " + g().cfg.code + " -- in TowerTrainer tick Settings > XRadio > X-Plane tower link";
        } else {
            s.linkStatus = type + ": " + e;
        }
        logf("%s", s.linkStatus.c_str());
    } else if (type == "radio") {
        // relayed radio calls on my frequency: a live feed under the trainer's log (which carries them a moment later)
        const json::Value& r = m.o("m");
        const std::string pos = r.s("pos"), my = myPosition();
        if (pos.empty() || pos == my || pos == "ALL" || pos == "GUARD") {
            LiveCall c;
            c.atc = r.s("from") == "atc";
            c.from = c.atc ? std::string("ATC") : r.o("ac").s("cs", "?");
            c.text = r.s("text");
            c.at = g().elapsed;
            s.live.push_back(c);
            if (s.live.size() > 8) s.live.erase(s.live.begin());
        }
    } else if (type == "hostLeft") { g().hostAway = true; s.linkStatus = "the trainer's window is away"; }
    else if (type == "hostBack") { g().hostAway = false; s.linkStatus = "linked"; sendClaim(); }
}

// ------------------------------------------------------------------ camera
void computeCab() {
    G& G_ = g();
    AptView av;
    { std::lock_guard<std::mutex> lk(G_.aptMx); av = G_.aptView; }
    if (av.found) { G_.cabLat = av.lat; G_.cabLon = av.lon; G_.cabHeightM = av.heightM > 3 ? av.heightM : 30.0; G_.cabFromApt = true; }
    else if (G_.st.haveTower) { G_.cabLat = latOf(G_.st.towerY); G_.cabLon = lonOf(G_.st.towerX); G_.cabHeightM = 30.0; G_.cabFromApt = false; }
    else if (G_.st.haveAirport) { G_.cabLat = G_.st.lat0; G_.cabLon = G_.st.lon0; G_.cabHeightM = 30.0; G_.cabFromApt = false; }
    if (G_.cfg.cabHeightM > 1.f) G_.cabHeightM = G_.cfg.cabHeightM;
    // ground under the cab, from the sim
    double x, y, z;
    XPLMWorldToLocal(G_.cabLat, G_.cabLon, 0.0, &x, &y, &z);
    XPLMProbeRef probe = XPLMCreateProbe(xplm_ProbeY);
    XPLMProbeInfo_t info; info.structSize = sizeof(info);
    double groundM = 0.0;
    if (probe && XPLMProbeTerrainXYZ(probe, (float)x, (float)y, (float)z, &info) == xplm_ProbeHitTerrain) {
        double la, lo, alt;
        XPLMLocalToWorld(info.locationX, info.locationY, info.locationZ, &la, &lo, &alt);
        groundM = alt;
    }
    if (probe) XPLMDestroyProbe(probe);
    G_.cabAltM = groundM + (G_.groundView ? 2.0 : G_.cabHeightM);
}

void aimAt(const Aircraft& a) {
    G& G_ = g();
    const double dx = (a.lon - G_.cabLon) * G_.st.kx, dy = (a.lat - G_.cabLat) * G_.st.ky;
    const double d = std::sqrt(dx * dx + dy * dy);
    G_.yaw = (float)(std::atan2(dx, dy) / kDegToRad);
    const double dz = a.altFt * 0.3048 - (G_.cabAltM - (G_.groundView ? 2.0 : G_.cabHeightM)) - G_.cabHeightM;
    G_.pitch = (float)(std::atan2(dz, std::max(1.0, d)) / kDegToRad);
}

int cameraCb(XPLMCameraPosition_t* out, int losing, void*) {
    G& G_ = g();
    if (losing) { G_.camOn = false; logf("X-Plane took the view back (change view or use Tower view to return)"); return 0; }
    if (!out) return 1;
    double x, y, z;
    XPLMWorldToLocal(G_.cabLat, G_.cabLon, G_.cabAltM, &x, &y, &z);
    out->x = (float)x; out->y = (float)y; out->z = (float)z;
    out->heading = G_.yaw; out->pitch = G_.pitch; out->roll = 0.f; out->zoom = G_.zoomF;
    return 1;
}

void takeCamera() {
    computeCab();
    XPLMControlCamera(xplm_ControlCameraUntilViewChanges, cameraCb, nullptr);
    g().camOn = true;
}

// ------------------------------------------------------------------ the user's aircraft
void findRefs() {
    Refs& r = g().refs;
    r.localX = XPLMFindDataRef("sim/flightmodel/position/local_x");
    r.localY = XPLMFindDataRef("sim/flightmodel/position/local_y");
    r.localZ = XPLMFindDataRef("sim/flightmodel/position/local_z");
    r.psi = XPLMFindDataRef("sim/flightmodel/position/psi");
    r.theta = XPLMFindDataRef("sim/flightmodel/position/theta");
    r.phi = XPLMFindDataRef("sim/flightmodel/position/phi");
    r.vx = XPLMFindDataRef("sim/flightmodel/position/local_vx");
    r.vy = XPLMFindDataRef("sim/flightmodel/position/local_vy");
    r.vz = XPLMFindDataRef("sim/flightmodel/position/local_vz");
    r.q = XPLMFindDataRef("sim/flightmodel/position/q");
    r.overridePath = XPLMFindDataRef("sim/operation/override/override_planepath");
    r.zulu = XPLMFindDataRef("sim/time/zulu_time_sec");
    r.visSm = XPLMFindDataRef("sim/weather/region/visibility_reported_sm");
    r.windDir = XPLMFindDataRef("sim/weather/region/wind_direction_degt");
    r.windSpd = XPLMFindDataRef("sim/weather/region/wind_speed_msc");
}

void parkAircraft() {
    G& G_ = g(); Refs& r = G_.refs; Parked& p = G_.parked;
    if (!r.localX) return;
    p.x = XPLMGetDatad(r.localX); p.y = XPLMGetDatad(r.localY); p.z = XPLMGetDatad(r.localZ);
    p.psi = XPLMGetDataf(r.psi); p.theta = XPLMGetDataf(r.theta); p.phi = XPLMGetDataf(r.phi);
    p.vx = XPLMGetDataf(r.vx); p.vy = XPLMGetDataf(r.vy); p.vz = XPLMGetDataf(r.vz);
    if (r.q) XPLMGetDatavf(r.q, p.q, 0, 4);
    int one = 1;
    if (r.overridePath) XPLMSetDatavi(r.overridePath, &one, 0, 1);
    p.on = true;
    logf("aircraft parked at %.1f %.1f %.1f (restored exactly on leaving)", p.x, p.y, p.z);
}

void holdParked() {
    G& G_ = g(); Refs& r = G_.refs; Parked& p = G_.parked;
    if (!p.on) return;
    XPLMSetDatad(r.localX, p.x); XPLMSetDatad(r.localY, p.y); XPLMSetDatad(r.localZ, p.z);
    XPLMSetDataf(r.vx, 0.f); XPLMSetDataf(r.vy, 0.f); XPLMSetDataf(r.vz, 0.f);
}

void restoreAircraft() {
    G& G_ = g(); Refs& r = G_.refs; Parked& p = G_.parked;
    if (!p.on) return;
    XPLMSetDatad(r.localX, p.x); XPLMSetDatad(r.localY, p.y); XPLMSetDatad(r.localZ, p.z);
    if (r.q) XPLMSetDatavf(r.q, p.q, 0, 4);
    XPLMSetDataf(r.psi, p.psi); XPLMSetDataf(r.theta, p.theta); XPLMSetDataf(r.phi, p.phi);
    XPLMSetDataf(r.vx, p.vx); XPLMSetDataf(r.vy, p.vy); XPLMSetDataf(r.vz, p.vz);
    int zero = 0;
    if (r.overridePath) XPLMSetDatavi(r.overridePath, &zero, 0, 1);
    p.on = false;
    logf("aircraft restored");
}

// ------------------------------------------------------------------ 3D traffic (standalone)
void updateTraffic() {
    G& G_ = g();
    const bool want = G_.active && G_.cfg.draw3d && !(G_.hooks.xradioConnected && G_.hooks.xradioConnected()) && csl::available();
    if (!want) {
        if (G_.drewAny) { for (auto& kv : G_.drawn) csl::remove(kv.second); G_.drawn.clear(); G_.drewAny = false; }
        return;
    }
    const float renderT = G_.elapsed - 0.3f;           // a little playout delay smooths the 4 Hz updates
    std::map<int, uint32_t> still;
    for (const auto& a : G_.st.ac) {
        if (a.veh || a.type.empty()) continue;
        auto it = G_.tracks.find(a.id);
        if (it == G_.tracks.end()) continue;
        const Track& t = it->second;
        float f = 1.f;
        if (t.b.t > t.a.t) f = std::max(0.f, std::min(1.5f, (renderT - t.a.t) / (t.b.t - t.a.t)));
        RemoteState s;
        s.sid = kSidBase | (uint32_t)(a.id & 0xFFFF);
        s.callsign = a.cs; s.acIcao = a.type;
        const float x = t.a.x + (t.b.x - t.a.x) * f, y = t.a.y + (t.b.y - t.a.y) * f;
        s.lat = latOf(y); s.lon = lonOf(x);
        s.altFt = t.a.altFt + (t.b.altFt - t.a.altFt) * f;
        float dh = t.b.hdg - t.a.hdg; while (dh > 180) dh -= 360; while (dh < -180) dh += 360;
        s.heading = t.a.hdg + dh * std::min(1.f, f);
        s.track = s.heading; s.gsKt = a.gsKt; s.onGround = a.onGround;
        s.gear = a.onGround || a.altFt < 2500 ? 1.f : 0.f;
        s.flap = a.onGround && a.gsKt > 30 ? 0.5f : (!a.onGround && a.altFt < 3000 ? 0.7f : 0.f);
        s.lights = a.onGround && a.gsKt < 1 ? 0x01 : 0x0F;
        s.squawk = (uint16_t)atoi(a.squawk.c_str());
        s.xpdrMode = a.onGround ? 1 : 3;
        s.timeMs = (uint32_t)(G_.elapsed * 1000.f);
        csl::upsert(s);
        still[a.id] = s.sid;
    }
    for (auto& kv : G_.drawn) if (!still.count(kv.first)) csl::remove(kv.second);
    G_.drawn.swap(still);
    G_.drewAny = !G_.drawn.empty();
    for (auto it = G_.tracks.begin(); it != G_.tracks.end();) {
        if (G_.elapsed - it->second.b.t > 10.f) it = G_.tracks.erase(it); else ++it;
    }
}

// ------------------------------------------------------------------ weather / time
void syncWeather() {
    G& G_ = g();
    if (!G_.cfg.syncWeather || !G_.active || G_.st.metar.empty() || G_.elapsed - G_.lastWx < 60.f) return;
    G_.lastWx = G_.elapsed;
    const Wx w = parseMetar(G_.st.metar);
    if (!w.ok) return;
    if (G_.refs.visSm) XPLMSetDataf(G_.refs.visSm, w.visM / 1609.34f);
    if (G_.refs.windDir && G_.refs.windSpd) {
        float d = w.windDeg, s = w.windKt * 0.514444f;
        XPLMSetDatavf(G_.refs.windDir, &d, 0, 1);
        XPLMSetDatavf(G_.refs.windSpd, &s, 0, 1);
    }
    if (G_.refs.zulu && G_.st.utcMs > 0) {
        const long long secOfDay = (G_.st.utcMs / 1000) % 86400;
        XPLMSetDataf(G_.refs.zulu, (float)secOfDay);
    }
    logf("weather from the trainer: vis %.0f m, wind %03.0f/%.0f kt", w.visM, w.windDeg, w.windKt);
}

void sendOps(const std::string& what, const std::string& valueJson) {
    sendLine("{\"type\":\"ops\",\"what\":" + json::quote(what) + ",\"value\":" + valueJson + "}");
}

void startLink() {
    G& G_ = g();
    G_.sessionMissing = false;
    G_.hostAway = false;
    G_.link.start(G_.cfg);
}

}  // namespace

// ------------------------------------------------------------------ apt.dat
AptView parseAptTower(const std::string& text, const std::string& icao) {
    AptView out;
    std::istringstream in(text);
    std::string line;
    bool inBlock = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const char c0 = line[0];
        if ((c0 == '1' && (line.size() == 1 || line[1] == ' ' || line[1] == '\t')) || line.rfind("99", 0) == 0) {
            if (inBlock) break;
            std::istringstream ls(line);
            std::string code, elev, a, b, code4;
            ls >> code >> elev >> a >> b >> code4;
            inBlock = code4 == icao;
            continue;
        }
        if (!inBlock) continue;
        if (line.rfind("14 ", 0) == 0 || line.rfind("14\t", 0) == 0) {
            std::istringstream ls(line);
            std::string code; double lat, lon; float h; int draw;
            if (ls >> code >> lat >> lon >> h >> draw) {
                std::string name; std::getline(ls, name);
                size_t s = name.find_first_not_of(" \t");
                out.found = true; out.lat = lat; out.lon = lon; out.heightM = h; out.name = s == std::string::npos ? "" : name.substr(s);
                return out;
            }
        }
    }
    return out;
}

AptView findAptTower(const std::string& root, const std::string& icao) {
    AptView out;
    if (root.empty() || icao.empty()) return out;
    std::vector<std::string> files;
    std::ifstream packs(root + "/Custom Scenery/scenery_packs.ini");
    std::string l;
    while (std::getline(packs, l)) {
        if (l.rfind("SCENERY_PACK ", 0) != 0) continue;
        std::string p = l.substr(13);
        while (!p.empty() && (p.back() == '\r' || p.back() == ' ' || p.back() == '/' || p.back() == '\\')) p.pop_back();
        if (p.rfind("Custom Scenery", 0) == 0) p = root + "/" + p;
        files.push_back(p + "/Earth nav data/apt.dat");
    }
    files.push_back(root + "/Global Scenery/Global Airports/Earth nav data/apt.dat");
    files.push_back(root + "/Resources/default scenery/default apt dat/Earth nav data/apt.dat");
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        // read the block of this airport only: find "1 ... ICAO" then up to the next airport row
        std::string line, block;
        bool inBlock = false;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (line[0] == '1' && (line.size() == 1 || line[1] == ' ' || line[1] == '\t')) {
                if (inBlock) break;
                std::istringstream ls(line);
                std::string code, elev, a, b, code4;
                ls >> code >> elev >> a >> b >> code4;
                if (code4 == icao) { inBlock = true; block = line + "\n"; }
                continue;
            }
            if (inBlock) { block += line; block += "\n"; if (block.size() > 4 * 1024 * 1024) break; }
        }
        if (inBlock) {
            out = parseAptTower(block, icao);
            if (out.found) return out;
            // a custom airport without a viewpoint hides the global one: keep looking
        }
    }
    return out;
}

Wx parseMetar(const std::string& metar) {
    Wx w;
    std::istringstream in(metar);
    std::string tok, prev;
    bool sawIcao = false;
    while (in >> tok) {
        const std::string before = prev;
        prev = tok;
        if (!sawIcao) { if (tok.size() == 4 && isalpha((unsigned char)tok[0])) sawIcao = true; continue; }
        if (tok.size() >= 7 && tok.compare(tok.size() - 2, 2, "KT") == 0) {
            const std::string d = tok.substr(0, 3);
            if (d == "VRB" || isdigit((unsigned char)d[0])) {
                w.windDeg = d == "VRB" ? 0.f : (float)atoi(d.c_str());
                w.windKt = (float)atoi(tok.substr(3, 2).c_str());
                const size_t g = tok.find('G');
                if (g != std::string::npos) w.gustKt = (float)atoi(tok.substr(g + 1, 2).c_str());
                w.ok = true;
            }
            continue;
        }
        if (tok == "CAVOK") { w.visM = 10000.f; w.ok = true; continue; }
        if (tok.size() == 4 && isdigit((unsigned char)tok[0]) && isdigit((unsigned char)tok[1]) && isdigit((unsigned char)tok[2]) && isdigit((unsigned char)tok[3]) && !w.ok) { continue; }
        if (tok.size() == 4 && std::all_of(tok.begin(), tok.end(), [](char c) { return isdigit((unsigned char)c); }) && w.ok && w.visM == 10000.f) { w.visM = (float)atoi(tok.c_str()); if (w.visM == 9999.f) w.visM = 10000.f; continue; }
        if (tok.size() >= 3 && tok.compare(tok.size() - 2, 2, "SM") == 0 && w.ok) {
            std::string v = tok.substr(0, tok.size() - 2);
            float sm = 0.f;
            if (v.find('/') != std::string::npos) { const int a = atoi(v.c_str()), b = atoi(v.c_str() + v.find('/') + 1); if (b) sm = (float)a / b; }
            else sm = (float)atof(v.c_str());
            // "1 1/2SM": the whole miles came as the token before
            if (v.find('/') != std::string::npos && before.size() <= 2 && !before.empty() && std::all_of(before.begin(), before.end(), [](char c) { return isdigit((unsigned char)c); })) sm += (float)atoi(before.c_str());
            w.visM = sm * 1609.34f;
        }
    }
    return w;
}

// ------------------------------------------------------------------ public API
void init(const Hooks& hooks) {
    g().hooks = hooks;
    findRefs();
}

void applyConfig(const Config& cfg) {
    G& G_ = g();
    const bool linkChanged = cfg.host != G_.cfg.host || cfg.port != G_.cfg.port || cfg.code != G_.cfg.code || cfg.name != G_.cfg.name;
    const bool posChanged = cfg.position != G_.cfg.position;
    G_.cfg = cfg;
    if (G_.active && linkChanged) startLink();
    else if (G_.active && posChanged) sendClaim();
    if (G_.active && !G_.cfg.showWindows && G_.windowsOn) setWindowsVisible(false);
}

void shutdown() {
    G& G_ = g();
    if (G_.active) leave();
    G_.link.stop();
    if (G_.aptThread.joinable()) G_.aptThread.join();
}

// The windows are the user interface's (ui2): this is only whether they should be open.
void setWindowsVisible(bool on) { g().windowsOn = on; }
bool windowsVisible() { return g().windowsOn; }

void startAptScan() {
    G& G_ = g();
    if (!G_.hooks.xplaneRoot || G_.st.icao.empty() || G_.aptScanned == G_.st.icao || G_.aptPending) return;
    G_.aptPending = true;
    const std::string root = G_.hooks.xplaneRoot(), icao = G_.st.icao;
    if (G_.aptThread.joinable()) G_.aptThread.join();
    G_.aptThread = std::thread([root, icao] {
        AptView v = findAptTower(root, icao);
        std::lock_guard<std::mutex> lk(g().aptMx);
        g().aptView = v;
        g().aptScanned = icao;
        g().aptPending = false;
    });
}

void tick(float dt, float elapsed) {
    (void)dt;
    G& G_ = g();
    G_.elapsed = elapsed;
    State& s = G_.st;
    s.now = elapsed;
    if (!G_.active) { s.phase = Phase::Off; return; }
    const bool wasLinked = s.linked;
    s.linked = G_.link.connected();
    if (!s.linked) s.linkStatus = G_.link.status();
    else if (!wasLinked) s.linkStatus = "linked";
    s.endpoint = G_.cfg.host + ":" + G_.cfg.port;
    s.linkError = G_.link.error();
    s.retryInS = G_.link.retryIn();
    s.linkAttempts = G_.link.attempts();
    s.pttDown = G_.hooks.pttDown && G_.hooks.pttDown();
    for (const auto& l : G_.link.drain()) handleLine(l);
    // A trainer that stopped answering outranks a session error it gave while it was there.
    s.phase = G_.link.unreachable() ? Phase::Unreachable
            : G_.sessionMissing ? Phase::NoSession
            : s.linked ? (G_.hostAway ? Phase::HostAway : Phase::Linked)
            : Phase::Connecting;
    if (elapsed - G_.lastPing > 10.f) { G_.lastPing = elapsed; sendLine("{\"type\":\"ping\",\"t\":" + std::to_string((long long)(elapsed * 1000)) + "}"); }
    if (s.haveAirport) {
        startAptScan();
        bool aptNow = false;
        { std::lock_guard<std::mutex> lk(G_.aptMx); aptNow = G_.aptView.found && G_.aptScanned == s.icao && !G_.cabFromApt; }
        static bool placed = false;
        if (G_.camOn && (!placed || aptNow)) {
            computeCab();
            if (!placed && !s.runways.empty()) {
                const Runway& r = s.runways[0];
                const double mx = (r.ends[0].x + r.ends[1].x) / 2.0, my = (r.ends[0].y + r.ends[1].y) / 2.0;
                const double dx = (lonOf((float)mx) - G_.cabLon) * s.kx, dy = (latOf((float)my) - G_.cabLat) * s.ky;
                G_.yaw = (float)(std::atan2(dx, dy) / kDegToRad);
            }
            placed = true;
        }
        if (!G_.active) placed = false;
    }
    if (G_.locked >= 0) { const Aircraft* a = findAc(G_.locked); if (a) aimAt(*a); else G_.locked = -1; }
    s.followId = G_.locked;
    holdParked();
    updateTraffic();
    syncWeather();
}

bool active() { return g().active; }

void enter() {
    G& G_ = g();
    if (G_.active) return;
    G_.active = true;
    G_.groundView = false;
    G_.locked = -1;
    G_.zoomF = 1.f; G_.pitch = -3.f;
    G_.st = State();
    G_.st.phase = Phase::Connecting;
    G_.st.endpoint = G_.cfg.host + ":" + G_.cfg.port;
    G_.tracks.clear();
    startLink();
    if (G_.cfg.park) parkAircraft();
    takeCamera();
    if (G_.cfg.showWindows) setWindowsVisible(true);
    logf("entered (TowerTrainer at %s:%s, session %s, position %s)", G_.cfg.host.c_str(), G_.cfg.port.c_str(), G_.cfg.code.c_str(), G_.cfg.position.c_str());
}

void leave() {
    G& G_ = g();
    if (!G_.active) return;
    G_.active = false;
    if (G_.camOn) { XPLMDontControlCamera(); G_.camOn = false; }
    restoreAircraft();
    updateTraffic();
    setWindowsVisible(false);
    G_.link.stop();
    G_.st.linked = false;
    G_.st.phase = Phase::Off;
    G_.st.linkStatus = "off";
    logf("left");
}

void toggle() { if (g().active) leave(); else enter(); }

void groundView() {
    G& G_ = g();
    if (!G_.active) return;
    G_.groundView = true; G_.locked = -1;
    if (!G_.camOn) { XPLMControlCamera(xplm_ControlCameraUntilViewChanges, cameraCb, nullptr); G_.camOn = true; }
    computeCab();
    G_.pitch = 2.f;
}
void towerView() {
    G& G_ = g();
    if (!G_.active) return;
    G_.groundView = false;
    if (!G_.camOn) { XPLMControlCamera(xplm_ControlCameraUntilViewChanges, cameraCb, nullptr); G_.camOn = true; }
    computeCab();
    G_.pitch = -3.f;
}
void look(float dYaw, float dPitch) {
    G& G_ = g();
    G_.yaw += dYaw; while (G_.yaw >= 360.f) G_.yaw -= 360.f; while (G_.yaw < 0.f) G_.yaw += 360.f;
    G_.pitch = std::max(-80.f, std::min(60.f, G_.pitch + dPitch));
    G_.locked = -1;
}
void zoom(float factor) { g().zoomF = std::max(0.5f, std::min(12.f, g().zoomF * factor)); }
void nextTarget() {
    G& G_ = g();
    if (G_.st.ac.empty()) return;
    size_t i = 0;
    for (; i < G_.st.ac.size(); ++i) if (G_.st.ac[i].id == G_.locked) break;
    for (size_t k = 1; k <= G_.st.ac.size(); ++k) { const Aircraft& a = G_.st.ac[(i + k) % G_.st.ac.size()]; if (!a.veh) { G_.locked = a.id; G_.st.selectedId = a.id; return; } }
}
void follow(int id) {
    G& G_ = g();
    G_.locked = id >= 0 && findAc(id) ? id : -1;
    G_.st.followId = G_.locked;
}
void lookAt(int id) {
    const Aircraft* a = findAc(id);
    if (!a || !g().active) return;
    g().locked = -1;
    aimAt(*a);
}

// ------------------------------------------------------------------ what the windows do
void select(int id) { g().st.selectedId = id >= 0 && findAc(id) ? id : -1; }

void transmit(const std::string& text) {
    G& G_ = g();
    const std::string t = trim(text);
    if (t.empty()) return;
    sendLine("{\"type\":\"tx\",\"pos\":" + json::quote(myPosition()) + ",\"text\":" + json::quote(t) + "}");
    LiveCall c; c.from = "ATC"; c.text = t; c.at = G_.elapsed; c.mine = true; c.atc = true;
    G_.st.live.push_back(c);      // my own call, until the trainer's log carries it
    if (G_.st.live.size() > 8) G_.st.live.erase(G_.st.live.begin());
}

void acceptHandoff(int id) {
    if (!findAc(id)) return;
    sendLine("{\"type\":\"strip\",\"id\":" + std::to_string(id) + ",\"accept\":true}");
}

void moveStrip(int id, const std::string& bay) {
    if (!findAc(id) || bay.empty()) return;
    sendLine("{\"type\":\"strip\",\"id\":" + std::to_string(id) + ",\"bay\":" + json::quote(bay) + "}");
}

void setLvp(bool on) { sendOps("lvp", on ? "true" : "false"); }
void setRunway(const std::string& end) { if (!end.empty()) sendOps("runway", json::quote(end)); }
void setStopBars(bool on) { sendOps("stopbars", on ? "true" : "false"); }

void lightGun(int id, const std::string& signal) {
    const Aircraft* a = findAc(id);
    if (a && !signal.empty()) transmit(a->cs + " light " + signal);
}

void retry() {
    G& G_ = g();
    if (!G_.active) return;
    G_.link.kick();
    logf("retrying the link to %s:%s now", G_.cfg.host.c_str(), G_.cfg.port.c_str());
}

std::string statusLine() {
    G& G_ = g();
    if (!G_.active) return "";
    char b[200];
    snprintf(b, sizeof b, "Tower %s: %s%s%s, %d aircraft", G_.st.icao.empty() ? "mode" : G_.st.icao.c_str(), G_.st.linked ? "linked" : G_.st.linkStatus.c_str(),
             G_.st.myPos.empty() ? "" : ", working ", G_.st.myPos.c_str(), (int)G_.st.ac.size());
    return b;
}
const State& state() { return g().st; }
CamView camera() {
    G& G_ = g();
    CamView v; v.on = G_.camOn; v.yaw = G_.yaw; v.pitch = G_.pitch; v.zoom = G_.zoomF; v.lat = G_.cabLat; v.lon = G_.cabLon; v.altM = G_.cabAltM; v.locked = G_.locked;
    return v;
}
const Config& config() { return g().cfg; }

}  // namespace tower
}  // namespace xr
