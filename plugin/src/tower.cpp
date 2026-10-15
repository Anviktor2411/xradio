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
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h"

#include "json.h"
#include "tcp.h"
#include "ui.h"
#include "xpmp_bridge.h"

namespace xr {
namespace tower {

namespace {

constexpr float kDegToRad = 3.14159265f / 180.f;
constexpr uint32_t kSidBase = 0x7E000000u;        // CSL ids of the trainer's traffic, away from XRadio's
constexpr int kMaxLog = 14;

// ------------------------------------------------------------------ the link thread
class Link {
public:
    void start(const Config& cfg) {
        stop();
        cfg_ = cfg;
        run_ = true;
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        run_ = false;
        sock_.close();
        if (th_.joinable()) th_.join();
        connected_ = false;
    }
    bool running() const { return run_; }
    bool connected() const { return connected_; }
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
    int linesIn() const { return linesIn_; }

private:
    void setStatus(const std::string& s) { std::lock_guard<std::mutex> lk(mx_); status_ = s; }
    void loop() {
        int backoffMs = 1000;
        while (run_) {
            std::string err;
            setStatus("connecting to " + cfg_.host + ":" + cfg_.port);
            if (!sock_.connect(cfg_.host, (uint16_t)atoi(cfg_.port.c_str()), 3000, &err)) {
                setStatus("TowerTrainer not reachable at " + cfg_.host + ":" + cfg_.port + " (" + err + ") -- is server.py running?");
                for (int i = 0; i < backoffMs / 100 && run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                backoffMs = std::min(10000, backoffMs * 2);
                continue;
            }
            backoffMs = 1000;
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
                    ++linesIn_;
                }
                if (buf.size() > 8 * 1024 * 1024) buf.clear();
            }
            connected_ = false;
            sock_.close();
            if (run_) setStatus("link lost -- reconnecting");
            for (int i = 0; i < 20 && run_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    Config                  cfg_;
    TcpSocket               sock_;
    std::thread             th_;
    std::atomic<bool>       run_{false};
    std::atomic<bool>       connected_{false};
    std::atomic<int>        linesIn_{0};
    std::mutex              mx_;
    std::deque<std::string> inbox_, outbox_;
    std::string             status_;
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

struct Win {
    XPLMWindowID id = nullptr;
    int kind = 0;                  // 0 radar, 1 strips, 2 delivery, 3 log, 4 ops
    ui::Ctx ctx;
    bool dragging = false; int dragX = 0, dragY = 0; bool dragMoved = false;
    std::vector<std::pair<int, int>> rowAc;    // y -> aircraft id, for clicks
    std::vector<std::pair<int, std::pair<int, int>>> cellAc; // radar: (ac id, (col,row))
};

struct G {
    Hooks   hooks;
    Config  cfg;
    State   st;
    Link    link;
    Refs    refs;
    Parked  parked;
    bool    active = false;
    bool    windowsOn = false;
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
    // windows
    Win     win[5];
    std::string cmd;
    int     radarRange = 0;        // 0 ground (3 km), 1 tower (15 nm)
    bool    windowsCreated = false;
};
G& g() { static G inst; return inst; }

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g().hooks.log) g().hooks.log(std::string("tower: ") + buf);
}

inline double latOf(float y) { return g().st.lat0 + y / g().st.ky; }
inline double lonOf(float x) { return g().st.lon0 + x / g().st.kx; }

// ------------------------------------------------------------------ messages from the trainer
void sendLine(const std::string& l) { g().link.send(l); }
void sendClaim() {
    if (!g().cfg.position.empty()) sendLine("{\"type\":\"claim\",\"pos\":" + json::quote(g().cfg.position) + "}");
}

std::vector<std::string> htmlLines(const std::string& html) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < html.size()) {
        size_t e = html.find("</div>", pos);
        std::string part = html.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        std::string t = json::stripHtml(part);
        if (!t.empty()) out.push_back(t);
        if (e == std::string::npos) break;
        pos = e + 6;
    }
    return out;
}

void onAirport(const json::Value& m) {
    State& s = g().st;
    s.icao = m.s("icao");
    s.aptName = m.s("name");
    s.lat0 = m.n("lat"); s.lon0 = m.n("lon");
    s.kx = cos(s.lat0 * kDegToRad) * 111320.0;
    s.ky = 110540.0;
    s.runways.clear();
    for (size_t i = 0; i < m.a("runways").size(); ++i) {
        const json::Value& r = m.a("runways")[i];
        Runway rw; rw.name = r.s("name"); rw.width = (float)r.n("width", 45);
        for (size_t k = 0; k < r.a("ends").size(); ++k) {
            const json::Value& e = r.a("ends")[k];
            rw.ends.push_back({e.s("name"), (float)e.n("x"), (float)e.n("y")});
        }
        if (rw.ends.size() == 2) s.runways.push_back(rw);
    }
    s.holdLines.clear();
    for (size_t i = 0; i < m.a("holdLines").size(); ++i) s.holdLines.emplace_back((float)m.a("holdLines")[i].n("x"), (float)m.a("holdLines")[i].n("y"));
    const json::Value* tw = m.at("tower");
    s.haveTower = tw && tw->isArray() && tw->size() >= 2;
    if (s.haveTower) { s.towerX = (float)(*tw)[0].num; s.towerY = (float)(*tw)[1].num; }
    s.haveAirport = true;
    logf("airport %s (%s) datum %.5f %.5f, %d runways, tower %s", s.icao.c_str(), s.aptName.c_str(), s.lat0, s.lon0, (int)s.runways.size(), s.haveTower ? "from OSM" : "unknown");
}

void onState(const json::Value& m) {
    State& s = g().st;
    s.rwy = m.s("rwy");
    s.lvp = m.truthy("lvp");
    s.atis = json::stripHtml(m.s("atis"));
    s.metar = m.s("metar");
    s.hostNick = m.s("hostNick");
    s.utcMs = (long long)m.n("utc");
    const json::Value* claims = m.at("claims");
    if (claims && !g().st.myId.empty()) { const json::Value* mine = claims->at(g().st.myId.c_str()); if (mine && mine->isString()) s.myPos = mine->str; }
    s.positionFreq.clear();
    for (const auto& kv : m.o("positions").obj) s.positionFreq[kv.first] = kv.second.s("freq");
    const std::string key = !s.myPos.empty() ? s.myPos : (m.truthy("split") ? g().cfg.position : "ALL");
    s.logLines.clear();
    const json::Value& lg = m.o("logs").a(key.c_str());
    const size_t n = lg.size();
    for (size_t i = n > (size_t)kMaxLog ? n - kMaxLog : 0; i < n; ++i) {
        std::string t = json::stripHtml(lg[i].s("html"));
        if (!t.empty()) s.logLines.push_back(t);
    }
    s.delLines = htmlLines(m.s("del"));
    std::vector<Aircraft> list;
    const json::Value& ac = m.a("ac");
    bool selStill = false;
    for (size_t i = 0; i < ac.size(); ++i) {
        const json::Value& a = ac[i];
        Aircraft r;
        r.id = (int)a.n("id");
        r.cs = a.s("cs"); r.type = a.s("type"); r.kind = a.s("kind"); r.state = a.s("state"); r.freq = a.s("freq"); r.squawk = a.s("squawk");
        const json::Value& other = a.a("other");
        r.dest = other.size() ? other[0].str : "";
        r.x = (float)a.a("pos")[0].num; r.y = (float)a.a("pos")[1].num;
        r.lat = latOf(r.y); r.lon = lonOf(r.x);
        r.altFt = (float)a.n("alt"); r.hdg = (float)a.n("hdg"); r.gsKt = (float)(a.n("spd") / 0.514444);
        r.onGround = a.truthy("onGround"); r.veh = a.truthy("veh"); r.human = a.truthy("human");
        const json::Value& h = a.o("handoff");
        if (h.isObject() && !h.truthy("accepted")) { r.handoffIn = h.s("to") == key; r.handoffFrom = h.s("from"); r.handoffAge = (int)h.n("age"); }
        if (r.id == s.selectedId) selStill = true;
        list.push_back(std::move(r));
    }
    s.ac.swap(list);
    if (!selStill) s.selectedId = -1;
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
        s.linkStatus = e.find("no such session") != std::string::npos
            ? "no session " + g().cfg.code + " -- in TowerTrainer tick Settings > XRadio > X-Plane tower link"
            : type + ": " + e;
        logf("%s", s.linkStatus.c_str());
    } else if (type == "radio") {
        // relayed radio calls on my frequency: a live feed under the trainer's log (which carries them a moment later)
        const json::Value& r = m.o("m");
        const std::string pos = r.s("pos"), my = !s.myPos.empty() ? s.myPos : g().cfg.position;
        if (pos.empty() || pos == my || pos == "ALL" || pos == "GUARD") {
            const std::string who = r.s("from") == "atc" ? std::string("ATC") : r.o("ac").s("cs", "?");
            s.live.emplace_back(who + ": " + r.s("text"), g().elapsed);
            if (s.live.size() > 6) s.live.erase(s.live.begin());
        }
    } else if (type == "hostLeft") s.linkStatus = "the trainer's window is away";
    else if (type == "hostBack") { s.linkStatus = "linked"; sendClaim(); }
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

// ------------------------------------------------------------------ windows
const char* kTitles[5] = {"Tower: radar", "Tower: strips", "Tower: Delivery", "Tower: frequency", "Tower: ATIS / operations"};

void sendTx(const std::string& text) {
    G& G_ = g();
    if (text.empty()) return;
    const std::string pos = !G_.st.myPos.empty() ? G_.st.myPos : G_.cfg.position;
    sendLine("{\"type\":\"tx\",\"pos\":" + json::quote(pos) + ",\"text\":" + json::quote(text) + "}");
    G_.st.live.emplace_back("> " + text, G_.elapsed);      // my own call, until the trainer's log carries it
    if (G_.st.live.size() > 6) G_.st.live.erase(G_.st.live.begin());
}
void sendOps(const std::string& what, const std::string& valueJson) {
    sendLine("{\"type\":\"ops\",\"what\":" + json::quote(what) + ",\"value\":" + valueJson + "}");
}
const Aircraft* selected() {
    for (const auto& a : g().st.ac) if (a.id == g().st.selectedId) return &a;
    return nullptr;
}
std::string fit(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "~"; }

// A character picture of the airport: runways as '=' and aircraft as the first letters of their callsign.
void drawRadar(Win& w) {
    G& G_ = g(); State& s = G_.st;
    ui::Ctx& c = w.ctx;
    const int cols = std::max(20, std::min(90, (c.right - c.left - 12) / 7)), rows = std::max(8, std::min(30, (c.top - c.bottom - 40) / 13));
    std::vector<std::string> grid((size_t)rows, std::string((size_t)cols, ' '));
    const float cx = s.haveTower ? s.towerX : 0.f, cy = s.haveTower ? s.towerY : 0.f;
    const float rangeM = G_.radarRange == 0 ? 3000.f : 15.f * 1852.f;
    auto cell = [&](float x, float y, int* col, int* row) {
        const float fx = (x - cx) / rangeM, fy = (y - cy) / rangeM;
        *col = (int)std::lround(cols / 2.0 + fx * cols / 2.0);
        *row = (int)std::lround(rows / 2.0 - fy * rows / 2.0);
        return *col >= 0 && *col < cols && *row >= 0 && *row < rows;
    };
    for (const auto& r : s.runways) {
        for (int i = 0; i <= 60; ++i) {
            const float f = i / 60.f;
            int col, row;
            if (cell(r.ends[0].x + (r.ends[1].x - r.ends[0].x) * f, r.ends[0].y + (r.ends[1].y - r.ends[0].y) * f, &col, &row)) grid[(size_t)row][(size_t)col] = '=';
        }
    }
    if (G_.radarRange == 0) for (const auto& h : s.holdLines) { int col, row; if (cell(h.first, h.second, &col, &row) && grid[(size_t)row][(size_t)col] == ' ') grid[(size_t)row][(size_t)col] = '.'; }
    int tc, tr;
    if (cell(cx, cy, &tc, &tr)) grid[(size_t)tr][(size_t)tc] = 'T';
    w.cellAc.clear();
    for (const auto& a : s.ac) {
        if (a.veh && G_.radarRange == 1) continue;
        if (G_.radarRange == 0 && !a.onGround && a.altFt > 3000) continue;
        int col, row;
        if (!cell(a.x, a.y, &col, &row)) continue;
        const std::string tag = a.cs.substr(0, (size_t)std::min<int>(4, (int)a.cs.size()));
        for (size_t i = 0; i < tag.size() && col + (int)i < cols; ++i) grid[(size_t)row][(size_t)(col + (int)i)] = tag[i];
        w.cellAc.emplace_back(a.id, std::make_pair(col, row));
    }
    char head[160];
    snprintf(head, sizeof head, "%s %s  range %s  %d aircraft  %s%s", s.icao.empty() ? "--" : s.icao.c_str(), s.rwy.empty() ? "" : ("RWY " + s.rwy).c_str(),
             G_.radarRange == 0 ? "3 km" : "15 nm", (int)s.ac.size(), G_.locked >= 0 ? "following " : "", G_.locked >= 0 ? (selected() ? selected()->cs.c_str() : "") : "");
    ui::text(c, head, 2);
    int fw = 7, fh = 10, dig = 0;
    XPLMGetFontDimensions(xplmFont_Basic, &fw, &fh, &dig);
    const int gridTop = c.y;
    float white[3] = {0.9f, 0.95f, 1.f}, green[3] = {0.4f, 1.f, 0.5f};
    for (int r = 0; r < rows; ++r) {
        XPLMDrawString(white, c.left + 6, gridTop - r * 13, (char*)grid[(size_t)r].c_str(), nullptr, xplmFont_Basic);
    }
    const Aircraft* sel = selected();
    if (sel) {
        int col, row;
        if (cell(sel->x, sel->y, &col, &row)) {
            std::string tag = sel->cs.substr(0, (size_t)std::min<int>(4, (int)sel->cs.size()));
            XPLMDrawString(green, c.left + 6 + col * fw, gridTop - row * 13, (char*)tag.c_str(), nullptr, xplmFont_Basic);
        }
    }
    // remember the geometry for clicks
    w.rowAc.clear();
    w.rowAc.emplace_back(gridTop, fw);           // misuse: first entry carries gridTop and the cell width
    w.rowAc.emplace_back(rows, cols);
    c.y = gridTop - rows * 13 - 6;
    const char* btn[2] = {"Range", "Follow / release"};
    const int b = ui::buttons(c, btn, 2);
    if (b == 0) G_.radarRange = 1 - G_.radarRange;
    if (b == 1) { if (G_.locked >= 0) G_.locked = -1; else if (sel) G_.locked = sel->id; }
    ui::text(c, "click an aircraft to select, drag to look around, wheel = binoculars", 1);
}

void drawStrips(Win& w) {
    G& G_ = g(); State& s = G_.st;
    ui::Ctx& c = w.ctx;
    const std::string my = !s.myPos.empty() ? s.myPos : G_.cfg.position;
    w.rowAc.clear();
    char head[120];
    snprintf(head, sizeof head, "Strips %s %s", my.c_str(), s.positionFreq.count(my) ? s.positionFreq[my].c_str() : "");
    ui::text(c, head, 2);
    const char* bays[4] = {"INCOMING", "DEPARTURES", "ARRIVALS", "OTHER"};
    for (int bay = 0; bay < 4; ++bay) {
        std::vector<const Aircraft*> list;
        for (const auto& a : s.ac) {
            if (a.veh) continue;
            const bool mine = a.freq == my || my == "ALL";
            if (bay == 0 ? a.handoffIn : (!mine || a.handoffIn) ? false : bay == 1 ? a.kind == "dep" : bay == 2 ? a.kind == "arr" : (a.kind != "dep" && a.kind != "arr")) list.push_back(&a);
        }
        if (list.empty() && bay != 1) continue;
        ui::text(c, bays[bay], 1);
        for (const Aircraft* a : list) {
            char row[160];
            snprintf(row, sizeof row, "%s%-8s %-4s %s%s %s%s", a->id == s.selectedId ? ">" : " ", a->cs.c_str(), a->type.c_str(), a->dest.empty() ? "" : a->dest.c_str(), a->dest.empty() ? "" : " ",
                     a->state.c_str(), a->handoffIn ? ("  [ACCEPT from " + a->handoffFrom + "]").c_str() : "");
            w.rowAc.emplace_back(c.y, a->id);
            ui::text(c, row, a->handoffIn ? 3 : a->id == s.selectedId ? 2 : 0);
            c.nextRow();
        }
    }
}

void drawDelivery(Win& w) {
    G& G_ = g(); State& s = G_.st;
    ui::Ctx& c = w.ctx;
    w.rowAc.clear();
    ui::text(c, "Delivery: clearance requests", 2);
    int n = 0;
    for (const auto& a : s.ac) {
        if (a.kind != "dep" || !(a.state == "clr_req" || a.state == "parked" || a.state == "cleared")) continue;
        char row[160];
        snprintf(row, sizeof row, "%s%-8s %-4s -> %-4s  sq %s  %s", a.id == s.selectedId ? ">" : " ", a.cs.c_str(), a.type.c_str(), a.dest.c_str(), a.squawk.c_str(), a.state == "clr_req" ? "REQUESTING" : a.state.c_str());
        w.rowAc.emplace_back(c.y, a.id);
        ui::text(c, row, a.state == "clr_req" ? 3 : a.id == s.selectedId ? 2 : 0);
        c.nextRow();
        if (++n >= 14) break;
    }
    for (const auto& l : s.delLines) { ui::text(c, fit(l, 90).c_str(), 1); if (++n >= 20) break; }
    if (n == 0) { ui::text(c, "nothing pending", 1); }
}

void drawLog(Win& w) {
    G& G_ = g(); State& s = G_.st;
    ui::Ctx& c = w.ctx;
    const std::string my = !s.myPos.empty() ? s.myPos : G_.cfg.position;
    const bool tx = G_.hooks.pttDown && G_.hooks.pttDown();
    char head[160];
    snprintf(head, sizeof head, "%s %s   %s", my.c_str(), s.positionFreq.count(my) ? s.positionFreq[my].c_str() : "", tx ? "TX *" : s.linked ? "RX" : s.linkStatus.c_str());
    ui::text(c, head, tx ? 3 : 2);
    const size_t maxCols = (size_t)std::max(30, (c.right - c.left - 12) / 7);
    for (const auto& l : s.logLines) { ui::text(c, fit(l, maxCols).c_str(), l.rfind("> ", 0) == 0 ? 2 : 0); }
    for (const auto& al : s.alerts) if (G_.elapsed - al.second < 20.f) { ui::text(c, fit("! " + al.first, maxCols).c_str(), 3); }
    for (const auto& lv : s.live) if (G_.elapsed - lv.second < 25.f) ui::text(c, fit(lv.first, maxCols).c_str(), lv.first.rfind("ATC", 0) == 0 || lv.first.rfind("> ", 0) == 0 ? 2 : 0);
    ui::text(c, ("Say: " + G_.cmd + ((int)(G_.elapsed * 2) % 2 ? "_" : " ")).c_str(), 2);
    ui::text(c, "Enter sends it as your instruction; the selected aircraft's callsign is filled in", 1);
}

void drawOps(Win& w) {
    G& G_ = g(); State& s = G_.st;
    ui::Ctx& c = w.ctx;
    char head[200];
    snprintf(head, sizeof head, "%s  RWY %s  LVP %s  %s", s.icao.c_str(), s.rwy.c_str(), s.lvp ? "ON" : "off", s.hostNick.empty() ? "" : ("host " + s.hostNick).c_str());
    ui::text(c, head, 2);
    const size_t maxCols = (size_t)std::max(30, (c.right - c.left - 12) / 7);
    std::string atis = s.atis.empty() ? "no ATIS yet" : s.atis;
    for (size_t i = 0; i < atis.size() && i < maxCols * 4; i += maxCols) { ui::text(c, atis.substr(i, maxCols).c_str(), 0); }
    if (!s.metar.empty()) { ui::text(c, fit(s.metar, maxCols).c_str(), 1); }
    const char* row1[4] = {s.lvp ? "LVP off" : "LVP on", "Next runway", "Stop bars on", "Stop bars off"};
    const int b1 = ui::buttons(c, row1, 4);
    if (b1 == 0) sendOps("lvp", s.lvp ? "false" : "true");
    if (b1 == 1) {
        std::vector<std::string> ends;
        for (const auto& r : s.runways) for (const auto& e : r.ends) ends.push_back(e.name);
        if (!ends.empty()) {
            size_t i = 0;
            for (; i < ends.size(); ++i) if (ends[i] == s.rwy) break;
            sendOps("runway", json::quote(ends[(i + 1) % ends.size()]));
        }
    }
    if (b1 == 2) sendOps("stopbars", "true");
    if (b1 == 3) sendOps("stopbars", "false");
    const Aircraft* sel = selected();
    char lg[120];
    snprintf(lg, sizeof lg, "Light gun%s%s:", sel ? " for " : "", sel ? sel->cs.c_str() : " (select an aircraft)");
    ui::text(c, lg, 1);
    const char* row2[5] = {"green steady", "green flashing", "red steady", "red flashing", "white flashing"};
    const int b2 = ui::buttons(c, row2, 5);
    if (b2 >= 0 && sel) sendTx(sel->cs + " light " + row2[b2]);
    ui::text(c, "TowerTrainer decides: these are requests to its simulation", 1);
}

// The window record of a callback: X-Plane passes the refcon, the test harness only the id.
Win* winOf(XPLMWindowID id, void* ref) {
    if (ref) return (Win*)ref;
    for (auto& w : g().win) if (w.id == id) return &w;
    return nullptr;
}

void drawCb(XPLMWindowID id, void* ref) {
    Win* wp = winOf(id, ref);
    if (!wp) return;
    Win& w = *wp;
    int l, t, r, b;
    XPLMGetWindowGeometry(id, &l, &t, &r, &b);
    w.ctx.begin(l, t, r, b, 18);
    switch (w.kind) {
        case 0: drawRadar(w); break;
        case 1: drawStrips(w); break;
        case 2: drawDelivery(w); break;
        case 3: drawLog(w); break;
        default: drawOps(w); break;
    }
    w.ctx.endInput();
}

void selectAircraft(int id) {
    G& G_ = g();
    G_.st.selectedId = id;
    const Aircraft* a = selected();
    if (a && (G_.cmd.empty() || G_.cmd.find(' ') == std::string::npos)) G_.cmd = a->cs + " ";
}

int clickCb(XPLMWindowID id, int x, int y, XPLMMouseStatus status, void* ref) {
    Win* wp = winOf(id, ref);
    if (!wp) return 1;
    Win& w = *wp;
    G& G_ = g();
    if (w.kind == 0) {
        if (status == xplm_MouseDown) { w.dragging = true; w.dragX = x; w.dragY = y; w.dragMoved = false; return 1; }
        if (status == xplm_MouseDrag && w.dragging) {
            const int dx = x - w.dragX, dy = y - w.dragY;
            if (abs(dx) + abs(dy) > 2) { w.dragMoved = true; look(dx * 0.25f, dy * 0.25f); w.dragX = x; w.dragY = y; }
            return 1;
        }
        if (status == xplm_MouseUp) {
            w.dragging = false;
            if (!w.dragMoved) {
                int l, t, r, b; XPLMGetWindowGeometry(id, &l, &t, &r, &b);
                if (w.rowAc.size() >= 2) {
                    const int gridTop = w.rowAc[0].first, fw = std::max(1, w.rowAc[0].second);
                    const int col = (x - l - 6) / fw, row = (gridTop + 4 - y) / 13;
                    int best = -1, bd = 99;
                    for (const auto& ca : w.cellAc) { const int d = abs(ca.second.first - col) + abs(ca.second.second - row) * 2; if (ca.second.second == row && d < bd) { bd = d; best = ca.first; } }
                    if (best >= 0 && bd <= 4) { selectAircraft(best); }
                    else { w.ctx.clicked = true; w.ctx.clickX = x; w.ctx.clickY = y; }
                }
            }
            return 1;
        }
        return 1;
    }
    if (status != xplm_MouseDown) return 1;
    for (const auto& ra : w.rowAc) {
        if (y >= ra.first - 6 && y <= ra.first + 15) {
            int l, t, r, b; XPLMGetWindowGeometry(id, &l, &t, &r, &b);
            if (w.kind == 1) {
                const Aircraft* a = nullptr;
                for (const auto& q : G_.st.ac) if (q.id == ra.second) a = &q;
                if (a && a->handoffIn && x > l + 330) { sendLine("{\"type\":\"strip\",\"id\":" + std::to_string(a->id) + ",\"accept\":true}"); return 1; }
            }
            selectAircraft(ra.second);
            return 1;
        }
    }
    w.ctx.clicked = true; w.ctx.clickX = x; w.ctx.clickY = y;
    return 1;
}

void keyCb(XPLMWindowID id, char key, XPLMKeyFlags flags, char vk, void* ref, int losingFocus) {
    Win* wp = winOf(id, ref);
    if (!wp) return;
    Win& w = *wp;
    if (losingFocus || !(flags & xplm_DownFlag)) return;
    const unsigned char k = (unsigned char)vk;
    if (k == 0x1B) { g().cmd.clear(); return; }
    if (w.kind != 3) return;
    if (k == 0x0D) { sendTx(g().cmd); g().cmd.clear(); return; }
    if (k == 0x08) { if (!g().cmd.empty()) g().cmd.pop_back(); return; }
    const unsigned char ch = (unsigned char)key;
    if (ch >= 0x20 && ch < 0x7F && g().cmd.size() < 160) g().cmd.push_back((char)ch);
}
int cursorCb(XPLMWindowID, int, int, void*) { return xplm_CursorDefault; }
int wheelCb(XPLMWindowID id, int, int, int, int clicks, void* ref) {
    Win* wp = winOf(id, ref);
    if (!wp) return 0;
    Win& w = *wp;
    if (w.kind == 0) { zoom(clicks > 0 ? 1.25f : 0.8f); return 1; }
    return 0;
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
    if (G_.active && linkChanged) G_.link.start(cfg);
    else if (G_.active && posChanged) sendClaim();
    if (G_.active && !G_.cfg.showWindows && G_.windowsOn) setWindowsVisible(false);
}

void createWindows() {
    G& G_ = g();
    if (G_.windowsCreated) return;
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const int W = std::max(900, sr - sl), H = std::max(600, st - sb);
    const int rects[5][4] = {
        {sl + 10, st - 40, sl + 10 + std::min(560, W / 2 - 20), st - 40 - std::min(330, H / 2 - 50)},          // radar, top left
        {sr - 10 - std::min(520, W / 2 - 20), st - 40, sr - 10, st - 40 - std::min(300, H / 2 - 50)},          // strips, top right
        {sl + 10, st - 40 - std::min(330, H / 2 - 50) - 10, sl + 10 + std::min(560, W / 2 - 20), sb + 190},    // delivery, left below radar
        {sl + 10, sb + 180, sl + 10 + std::min(620, W / 2), sb + 10},                                           // log + command, bottom left
        {sr - 10 - std::min(520, W / 2 - 20), st - 40 - std::min(300, H / 2 - 50) - 10, sr - 10, sb + 10},     // ops, right below strips
    };
    for (int i = 0; i < 5; ++i) {
        Win& w = G_.win[i];
        w.kind = i;
        XPLMCreateWindow_t p{};
        p.structSize = sizeof(p);
        p.left = rects[i][0]; p.top = rects[i][1]; p.right = rects[i][2]; p.bottom = rects[i][3];
        p.visible = 0;
        p.drawWindowFunc = drawCb;
        p.handleMouseClickFunc = clickCb;
        p.handleKeyFunc = keyCb;
        p.handleCursorFunc = cursorCb;
        p.handleMouseWheelFunc = wheelCb;
        p.refcon = &w;
        p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
        p.layer = xplm_WindowLayerFloatingWindows;
        w.id = XPLMCreateWindowEx(&p);
        if (w.id) { XPLMSetWindowTitle(w.id, kTitles[i]); XPLMSetWindowResizingLimits(w.id, 320, 120, 1600, 1200); }
    }
    G_.windowsCreated = true;
}

void shutdown() {
    G& G_ = g();
    if (G_.active) leave();
    G_.link.stop();
    if (G_.aptThread.joinable()) G_.aptThread.join();
    for (auto& w : G_.win) if (w.id) { XPLMDestroyWindow(w.id); w.id = nullptr; }
    G_.windowsCreated = false;
}

void setWindowsVisible(bool on) {
    G& G_ = g();
    G_.windowsOn = on;
    for (auto& w : G_.win) if (w.id) XPLMSetWindowIsVisible(w.id, on ? 1 : 0);
    if (on && G_.win[3].id) XPLMTakeKeyboardFocus(G_.win[3].id);
}
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
    if (!G_.active) return;
    const bool wasLinked = G_.st.linked;
    G_.st.linked = G_.link.connected();
    if (!G_.st.linked) G_.st.linkStatus = G_.link.status();
    else if (!wasLinked) G_.st.linkStatus = "linked";
    for (const auto& l : G_.link.drain()) handleLine(l);
    if (elapsed - G_.lastPing > 10.f) { G_.lastPing = elapsed; sendLine("{\"type\":\"ping\",\"t\":" + std::to_string((long long)(elapsed * 1000)) + "}"); }
    if (G_.st.haveAirport) {
        startAptScan();
        bool aptNow = false;
        { std::lock_guard<std::mutex> lk(G_.aptMx); aptNow = G_.aptView.found && G_.aptScanned == G_.st.icao && !G_.cabFromApt; }
        static bool placed = false;
        if (G_.camOn && (!placed || aptNow)) {
            computeCab();
            if (!placed && !G_.st.runways.empty()) {
                const Runway& r = G_.st.runways[0];
                const double mx = (r.ends[0].x + r.ends[1].x) / 2.0, my = (r.ends[0].y + r.ends[1].y) / 2.0;
                const double dx = (lonOf((float)mx) - G_.cabLon) * G_.st.kx, dy = (latOf((float)my) - G_.cabLat) * G_.st.ky;
                G_.yaw = (float)(std::atan2(dx, dy) / kDegToRad);
            }
            placed = true;
        }
        if (!G_.active) placed = false;
    }
    if (G_.locked >= 0) { const Aircraft* a = nullptr; for (const auto& q : G_.st.ac) if (q.id == G_.locked) a = &q; if (a) aimAt(*a); else G_.locked = -1; }
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
    G_.tracks.clear();
    G_.link.start(G_.cfg);
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
