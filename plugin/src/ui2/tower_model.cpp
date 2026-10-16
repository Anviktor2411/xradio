#include "tower_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "i18n.h"

namespace xr {
namespace ui2 {
namespace tw {

namespace {
std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
std::string firstWord(const std::string& s, size_t* end = nullptr) {
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) { if (end) *end = s.size(); return ""; }
    const size_t b = s.find(' ', a);
    if (end) *end = b == std::string::npos ? s.size() : b;
    return s.substr(a, (b == std::string::npos ? s.size() : b) - a);
}
bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }
}  // namespace

// ---------------------------------------------------------------- strips (TowerTrainer's strips.js)
std::vector<std::string> baysFor(const std::string& pos, bool split) {
    const std::string p = split ? pos : "ALL";
    if (p == "DEL") return {"incoming", "pending", "clearance"};
    if (p == "GND") return {"incoming", "clearance", "push", "taxi", "holding", "taxiin", "vehicles", "live"};
    if (p == "TWR") return {"incoming", "holding", "runway", "airborne", "final", "landed", "taxiin", "vehicles", "live"};
    if (p == "APP") return {"incoming", "approach", "final", "airborne", "live"};
    if (p == "CTR") return {"incoming", "enroute", "airborne", "approach", "live"};
    return {"pending", "clearance", "push", "taxi", "holding", "runway", "airborne", "approach", "final", "landed", "taxiin", "vehicles", "live"};
}

const char* bayTitle(const std::string& b) {
    static const struct { const char* id; const char* title; } k[] = {
        {"incoming", "Incoming"}, {"pending", "Pending"}, {"clearance", "Clearance"}, {"push", "Push"}, {"taxi", "Taxi"}, {"holding", "Holding"},
        {"runway", "Runway"}, {"airborne", "Airborne"}, {"approach", "Approach"}, {"final", "Final"}, {"landed", "Runway (arrivals)"},
        {"taxiin", "Taxi in"}, {"enroute", "Enroute"}, {"live", "Live pilots"}, {"vehicles", "Vehicles"},
    };
    for (const auto& e : k) if (b == e.id) return e.title;
    return "Other";
}

std::string autoBay(const tower::Aircraft& a) {
    if (a.human) return "live";
    if (a.veh) return "vehicles";
    if (a.kind == "ovf") return "enroute";
    const std::string& s = a.state;
    if (a.kind == "dep") {
        if (s == "parked" || s == "clr_req") return a.ga ? "taxi" : "pending";
        if (s == "cleared") return "clearance";
        if (s == "push_req" || s == "pushing" || s == "pushed") return "push";
        if (s == "taxi_req" || s == "taxi") return "taxi";
        if (s == "holding") return "holding";
        if (s == "lineup" || s == "lining" || s == "lined_up" || s == "roll") return "runway";
        return "airborne";
    }
    if (s == "final" || s == "goaround") return a.freq == "APP" ? "approach" : "final";
    if (s == "rollout" || s == "vacating") return "landed";
    return "taxiin";
}

std::string bayOf(const tower::Aircraft& a) {
    if (!a.stripBay.empty() && a.stripState == a.state) return a.stripBay;
    return autoBay(a);
}

bool incomingFor(const tower::Aircraft& a, const std::string& pos) {
    return !a.handoffTo.empty() && a.handoffTo == pos;
}

bool onBoard(const tower::Aircraft& a, const std::string& pos, bool split) {
    if (a.veh && a.state == "idle") return false;
    return !split || pos == "ALL" || a.freq == pos || (a.human && pos != "DEL") ||
           (pos == "DEL" && a.kind == "dep" && (a.state == "parked" || a.state == "clr_req")) || incomingFor(a, pos);
}

std::string shortState(const tower::Aircraft& a) {
    if (a.human) return "LIVE";
    if (a.veh) return a.state == "leading" ? "LEADING" : a.state == "toAircraft" ? "EN ROUTE" : "VEHICLE";
    static const struct { const char* s; const char* t; } k[] = {
        {"parked", "On stand"}, {"clr_req", "Clr req"}, {"cleared", "Cleared"}, {"push_req", "Push req"}, {"pushing", "Pushing"}, {"pushed", "Pushed"},
        {"taxi_req", "Taxi req"}, {"taxi", "Taxiing"}, {"holding", "Ready"}, {"lineup", "Line up"}, {"lining", "Line up"}, {"lined_up", "Lined up"},
        {"roll", "T/O roll"}, {"airborne", "Airborne"}, {"final", "Final"}, {"goaround", "Go-around"}, {"rollout", "Landed"}, {"vacating", "Vacating"},
        {"vacated", "Vacated"}, {"holding_short", "Hold short"}, {"cruise", "Enroute"}, {"circuit", "Circuit"}, {"rto", "RTO"},
    };
    for (const auto& e : k) if (a.state == e.s) return e.t;
    return a.state;
}

// ---------------------------------------------------------------- the command line
std::string withCallsign(const std::string& cmd, const std::string& cs) {
    if (cs.empty()) return cmd;
    size_t end = 0;
    const std::string w = firstWord(cmd, &end);
    const std::string rest = end < cmd.size() ? cmd.substr(end) : "";
    const bool restEmpty = rest.find_first_not_of(' ') == std::string::npos;
    if (w.empty() || restEmpty) return cs + " ";                     // nothing, or only a callsign so far
    return cmd;                                                       // an instruction is being written: leave it
}

std::vector<std::string> callsignMatches(const std::string& cmd, const std::vector<tower::Aircraft>& ac, size_t max) {
    std::vector<std::string> out;
    size_t end = 0;
    const std::string w = upper(firstWord(cmd, &end));
    if (w.empty() || end < cmd.size()) return out;                    // only while the first word is being typed
    std::vector<std::string> pre, mid;
    for (const auto& a : ac) {
        if (a.cs.empty()) continue;
        const std::string cs = upper(a.cs);
        if (cs == w) continue;
        if (cs.compare(0, w.size(), w) == 0) pre.push_back(a.cs);
        else if (cs.find(w) != std::string::npos) mid.push_back(a.cs);
    }
    std::sort(pre.begin(), pre.end());
    std::sort(mid.begin(), mid.end());
    for (const auto& v : {pre, mid}) for (const auto& s : v) if (out.size() < max) out.push_back(s);
    return out;
}

std::string completeCallsign(const std::string& cmd, const std::vector<tower::Aircraft>& ac) {
    const auto all = callsignMatches(cmd, ac, 64);
    if (all.size() == 1) return all[0] + " ";
    // several: as far as the ones that start with what is typed all agree
    const std::string w = upper(firstWord(cmd));
    std::vector<std::string> pre;
    for (const auto& s : all) if (upper(s).compare(0, w.size(), w) == 0) pre.push_back(s);
    if (pre.size() == 1) return pre[0] + " ";
    if (pre.empty()) return cmd;
    std::string common = pre[0];
    for (const auto& s : pre) {
        size_t i = 0;
        while (i < common.size() && i < s.size() && toupper((unsigned char)common[i]) == toupper((unsigned char)s[i])) ++i;
        common.resize(i);
    }
    return common.size() > w.size() ? common : cmd;
}

void History::push(const std::string& line) {
    pos_ = -1; draft_.clear();
    if (line.find_first_not_of(' ') == std::string::npos) return;
    if (!items_.empty() && items_.back() == line) return;
    items_.push_back(line);
    if (items_.size() > 50) items_.erase(items_.begin());
}

bool History::up(const std::string& current, std::string* out) {
    if (items_.empty()) return false;
    if (pos_ < 0) { draft_ = current; pos_ = (int)items_.size() - 1; }
    else if (pos_ > 0) --pos_;
    else return false;
    *out = items_[(size_t)pos_];
    return true;
}

bool History::down(std::string* out) {
    if (pos_ < 0) return false;
    if (pos_ + 1 < (int)items_.size()) { ++pos_; *out = items_[(size_t)pos_]; return true; }
    pos_ = -1;
    *out = draft_;
    return true;
}

// ---------------------------------------------------------------- ground radar
Vec toScreen(const View& v, Vec o, Vec size, float x, float y) {
    return {o.x + size.x * 0.5f + (x - v.cx) * v.ppm, o.y + size.y * 0.5f - (y - v.cy) * v.ppm};
}

Vec toWorld(const View& v, Vec o, Vec size, float sx, float sy) {
    return {v.cx + (sx - o.x - size.x * 0.5f) / v.ppm, v.cy - (sy - o.y - size.y * 0.5f) / v.ppm};
}

void zoomAt(View& v, Vec o, Vec size, float factor, Vec s) {
    const Vec w = toWorld(v, o, size, s.x, s.y);
    v.ppm = std::max(0.002f, std::min(4.f, v.ppm * factor));
    v.cx = w.x - (s.x - o.x - size.x * 0.5f) / v.ppm;
    v.cy = w.y + (s.y - o.y - size.y * 0.5f) / v.ppm;
}

void pan(View& v, float dx, float dy) {
    v.cx -= dx / v.ppm;
    v.cy += dy / v.ppm;
}

View fit(const tower::State& s, Vec size) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    auto add = [&](float x, float y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); };
    for (const auto& r : s.runways) for (const auto& e : r.ends) add(e.x, e.y);
    for (const auto& p : s.pavement) if (!p.empty()) for (const auto& q : p[0]) add(q.x, q.y);
    for (const auto& st : s.stands) add(st.x, st.y);
    for (const auto& n : s.taxiNodes) add(n.x, n.y);
    if (s.haveTower) add(s.towerX, s.towerY);
    View v;
    if (x1 < x0) { v.ppm = std::max(size.x, size.y) / 6000.f; return v; }   // nothing yet: 3 km around the datum
    // traffic on and right by the airport belongs in the picture too (not the arrival 10 miles out)
    const float ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1, near = 1500.f;
    for (const auto& a : s.ac) if (a.x > ax0 - near && a.x < ax1 + near && a.y > ay0 - near && a.y < ay1 + near) add(a.x, a.y);
    const float w = std::max(200.f, x1 - x0), h = std::max(200.f, y1 - y0);
    v.cx = (x0 + x1) * 0.5f; v.cy = (y0 + y1) * 0.5f;
    // room round the edges for the labels of what sits at the very end of a runway
    const float padX = 70.f, padY = 40.f;
    v.ppm = std::max(0.002f, std::min(std::max(40.f, size.x - 2 * padX) / w, std::max(40.f, size.y - 2 * padY) / h));
    return v;
}

float ringStep(float ppm) {
    static const float steps[] = {100, 200, 500, 1000, 2000, 5000, 1852 * 5, 1852 * 10, 1852 * 20, 1852 * 50};
    for (float s : steps) if (s * ppm >= 70.f) return s;
    return 1852 * 100;
}

std::string rangeText(float m) {
    char b[32];
    if (m >= 1852 * 5 - 1) snprintf(b, sizeof b, "%.0f nm", m / 1852.f);
    else if (m >= 1000) snprintf(b, sizeof b, "%.0f km", m / 1000.f);
    else snprintf(b, sizeof b, "%.0f m", m);
    return b;
}

float wingspanM(const std::string& wtc) {
    if (wtc == "L") return 12.f;
    if (wtc == "H") return 60.f;
    if (wtc == "J" || wtc == "S") return 80.f;
    return 34.f;
}

const std::vector<Vec>& planeShape() {
    // nose at +y, wingspan 1 (x from -0.5 to 0.5), fuselage about as long as the span
    static const std::vector<Vec> s = {
        {0.00f, 0.52f}, {0.05f, 0.44f}, {0.055f, 0.14f}, {0.50f, -0.06f}, {0.50f, -0.13f}, {0.055f, -0.04f}, {0.045f, -0.34f},
        {0.19f, -0.44f}, {0.19f, -0.49f}, {0.00f, -0.45f}, {-0.19f, -0.49f}, {-0.19f, -0.44f}, {-0.045f, -0.34f}, {-0.055f, -0.04f},
        {-0.50f, -0.13f}, {-0.50f, -0.06f}, {-0.055f, 0.14f}, {-0.05f, 0.44f},
    };
    return s;
}

int labelLines(const tower::Aircraft& a, bool selected, bool hovered, bool calling, float ppm, bool labelsOn) {
    if (selected || hovered) return 2;
    const bool trouble = !a.warn.empty() || a.handoffIn;
    if (a.quiet && !trouble && !calling) return 0;
    if (!a.veh && a.state == "parked" && !trouble && !calling && (ppm < 0.25f || a.dormant)) return ppm > 0.08f ? 1 : 0;
    return labelsOn ? 2 : 1;
}

int labelPriority(const tower::Aircraft& a, bool selected, bool calling) {
    if (selected) return 5;
    if (calling || a.handoffIn || !a.warn.empty()) return 4;
    if (a.veh) return 0;
    if (!a.onGround) return 3;
    return a.state == "parked" ? 1 : 2;
}

bool labelMayHide(const tower::Aircraft& a, bool selected, bool calling) {
    return labelPriority(a, selected, calling) <= 1;
}

// ---------------------------------------------------------------- the connection banner
Banner bannerFor(const tower::State& s, const std::string& code) {
    // the words in the window's language: templates through tr(), the values put in after
    Banner b;
    char t[320], d[320];
    const char* ep = s.endpoint.c_str();
    switch (s.phase) {
        case tower::Phase::Off: break;
        case tower::Phase::Connecting:
            b.show = true; b.level = 0;
            snprintf(t, sizeof t, tr("Connecting to TowerTrainer at %s\xE2\x80\xA6"), ep);
            b.title = t;
            break;
        case tower::Phase::Unreachable: {
            b.show = true; b.level = 1; b.retry = true;
            snprintf(t, sizeof t, tr("Start TowerTrainer (server.py) \xE2\x80\x93 waiting for %s\xE2\x80\xA6"), ep);
            b.title = t;
            const int in = (int)std::ceil(s.retryInS);
            if (!s.linkError.empty() && s.retryInS > 0.5f) snprintf(d, sizeof d, tr("No answer: %s. Trying again in %d s, or press Retry once it is running."), s.linkError.c_str(), in);
            else if (s.retryInS > 0.5f) snprintf(d, sizeof d, tr("No answer. Trying again in %d s, or press Retry once it is running."), in);
            else snprintf(d, sizeof d, "%s", tr("No answer yet. Trying again now."));
            b.detail = d;
            break;
        }
        case tower::Phase::NoSession:
            b.show = true; b.level = 1; b.retry = true;
            snprintf(t, sizeof t, tr("TowerTrainer is running, but it hosts no session %s"), code.c_str());
            snprintf(d, sizeof d, tr("In TowerTrainer tick Settings > XRadio > X-Plane tower link (session code %s). The plugin keeps trying."), code.c_str());
            b.title = t; b.detail = d;
            break;
        case tower::Phase::HostAway:
            b.show = true; b.level = 1;
            b.title = tr("TowerTrainer's window is away");
            b.detail = tr("Its simulation waits until the window is back; what you see here is from before.");
            break;
        case tower::Phase::Linked:
            if (!s.haveAirport) {
                b.show = true; b.level = 0;
                b.title = tr("Linked to TowerTrainer \xE2\x80\x93 waiting for its airport\xE2\x80\xA6");
                b.detail = tr("Open an airport in TowerTrainer's window.");
            }
            break;
    }
    return b;
}

// ---------------------------------------------------------------- the light gun
const std::vector<Signal>& signals() {
    static const std::vector<Signal> k = {
        {"green steady", "Steady green", 0.25f, 0.88f, 0.42f, false},
        {"green flashing", "Flashing green", 0.25f, 0.88f, 0.42f, true},
        {"red steady", "Steady red", 1.00f, 0.30f, 0.28f, false},
        {"red flashing", "Flashing red", 1.00f, 0.30f, 0.28f, true},
        {"white flashing", "Flashing white", 0.96f, 0.96f, 0.96f, true},
    };
    return k;
}

const char* signalMeaning(const std::string& id, bool onGround) {
    if (id == "green steady") return onGround ? "cleared for takeoff" : "cleared to land";
    if (id == "green flashing") return onGround ? "cleared to taxi" : "return for landing";
    if (id == "red steady") return onGround ? "stop" : "give way to other aircraft and continue circling";
    if (id == "red flashing") return onGround ? "taxi clear of the landing area" : "aerodrome unsafe, do not land";
    if (id == "white flashing") return onGround ? "return to the starting point" : "no meaning in the air";
    return "";
}

// ---------------------------------------------------------------- the frequency log
Sender senderOf(const tower::LogEntry& e) {
    const std::string& c = e.cls;
    if (c == "err" || c == "blk") return Sender::Problem;
    if (c == "atc") return Sender::Atc;
    if (c == "ai") return Sender::AiAtc;
    if (c.rfind("pilot", 0) == 0) {
        if (c.find("human") != std::string::npos) return Sender::LivePilot;
        if (c.find("veh") != std::string::npos) return Sender::Vehicle;
        if (c.find("dep") != std::string::npos) return Sender::Departure;
        if (c.find("arr") != std::string::npos) return Sender::Arrival;
        return Sender::Pilot;
    }
    return Sender::System;
}

std::string displayText(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = (unsigned char)in[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        if (i + n > in.size()) n = 1;
        unsigned cp = c;
        if (n == 2) cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F);
        else if (n == 3) cp = ((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F);
        else if (n == 4) cp = ((c & 0x07) << 18) | ((in[i + 1] & 0x3F) << 12) | ((in[i + 2] & 0x3F) << 6) | (in[i + 3] & 0x3F);
        if (cp >= 0x10000 || cp == 0xFE0F || cp == 0x200D) {           // emoji and their joiners: nothing to show
            i += n;
            if (!out.empty() && out.back() == ' ' && i < in.size() && in[i] == ' ') ++i;
            continue;
        }
        if (cp == 0x2713 || cp == 0x2714) out += "OK";                  // check marks
        else if (cp == 0x27FF || cp == 0x27F6) out += "\xC2\xBB";       // long arrows -> >>
        else out.append(in, i, n);
        i += n;
    }
    // what dropping left behind at the start
    const size_t a = out.find_first_not_of(' ');
    return a == std::string::npos ? "" : out.substr(a);
}

bool inLog(const tower::LiveCall& c, const std::vector<tower::LogEntry>& log) {
    if (c.text.empty()) return true;
    const size_t from = log.size() > 12 ? log.size() - 12 : 0;
    for (size_t i = from; i < log.size(); ++i) if (log[i].text.find(c.text) != std::string::npos) return true;
    return false;
}

std::string waitingText(int s) {
    if (s < 0) return "";
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%d s", s);
    else snprintf(b, sizeof b, "%d min", (s + 30) / 60);
    return b;
}

std::string levelText(const std::string& cfl) {
    if (cfl.empty() || cfl == "0") return "";
    std::string v = upper(cfl);
    if (v.rfind("FL", 0) == 0) return v;
    if (v[0] == 'F') return "FL" + v.substr(1);
    return v + " ft";
}

}  // namespace tw
}  // namespace ui2
}  // namespace xr
