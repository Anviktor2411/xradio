#include "tower_parse.h"

#include <cmath>

namespace xr {
namespace tower {

namespace {
constexpr double kDegToRad = 3.14159265358979 / 180.0;
constexpr size_t kMaxLog = 80;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
Pt point(const json::Value& v) { Pt p; if (v.isArray() && v.size() >= 2) { p.x = (float)v[0].num; p.y = (float)v[1].num; } return p; }
}  // namespace

LogEntry parseLogEntry(const std::string& cls, const std::string& html) {
    LogEntry e;
    e.cls = cls;
    std::string h = html;
    auto takeSpan = [&h](const char* open, std::string* inner) {
        const std::string o(open);
        if (h.compare(0, o.size(), o) != 0) return false;
        const size_t end = h.find("</span>", o.size());
        if (end == std::string::npos) return false;
        if (inner) *inner = json::stripHtml(h.substr(o.size(), end - o.size()));
        h.erase(0, end + 7);
        return true;
    };
    takeSpan("<span class=\"t\">", &e.time);
    e.guard = takeSpan("<span class=\"guard\">", nullptr);
    // the sender in bold, after at most a small tag (an icon) the trainer may put in front of it
    const size_t b = h.find("<b>");
    if (b != std::string::npos && trim(json::stripHtml(h.substr(0, b))).size() <= 8) {
        const size_t end = h.find("</b>", b);
        if (end != std::string::npos) {
            e.from = trim(json::stripHtml(h.substr(b + 3, end - b - 3)));
            h.erase(0, end + 4);
        }
    }
    e.text = trim(json::stripHtml(h));
    return e;
}

void applyAirport(State& s, const json::Value& m) {
    s.icao = m.s("icao");
    s.aptName = m.s("name");
    s.lat0 = m.n("lat"); s.lon0 = m.n("lon");
    s.kx = std::cos(s.lat0 * kDegToRad) * 111320.0;
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
    for (size_t i = 0; i < m.a("holdLines").size(); ++i) s.holdLines.push_back({(float)m.a("holdLines")[i].n("x"), (float)m.a("holdLines")[i].n("y")});
    s.stands.clear();
    for (size_t i = 0; i < m.a("stands").size(); ++i) {
        const json::Value& st = m.a("stands")[i];
        s.stands.push_back({st.s("name"), (float)st.n("x"), (float)st.n("y"), (float)st.n("hdg")});
    }
    // the taxi network and the pavement: what a ground radar draws (an older trainer sends neither)
    s.taxiNodes.clear(); s.taxiEdges.clear();
    const json::Value& tx = m.o("taxi");
    for (size_t i = 0; i < tx.a("nodes").size(); ++i) s.taxiNodes.push_back(point(tx.a("nodes")[i]));
    for (size_t i = 0; i < tx.a("edges").size(); ++i) {
        const json::Value& e = tx.a("edges")[i];
        if (!e.isArray() || e.size() < 2) continue;
        TaxiEdge te; te.a = (int)e[0].num; te.b = (int)e[1].num;
        if (e.size() > 2 && e[2].isString()) te.name = e[2].str;
        if (e.size() > 3 && e[3].isNumber()) te.widthM = (float)e[3].num;
        if (te.a >= 0 && te.b >= 0 && te.a < (int)s.taxiNodes.size() && te.b < (int)s.taxiNodes.size()) s.taxiEdges.push_back(te);
    }
    s.pavement.clear();
    for (size_t i = 0; i < m.a("pavement").size(); ++i) {
        const json::Value& poly = m.a("pavement")[i];
        Polygon pg;
        for (size_t k = 0; k < poly.size(); ++k) {
            Ring ring;
            for (size_t j = 0; j < poly[k].size(); ++j) ring.push_back(point(poly[k][j]));
            if (ring.size() >= 3) pg.push_back(std::move(ring));
        }
        if (!pg.empty()) s.pavement.push_back(std::move(pg));
    }
    const json::Value* tw = m.at("tower");
    s.haveTower = tw && tw->isArray() && tw->size() >= 2;
    if (s.haveTower) { s.towerX = (float)(*tw)[0].num; s.towerY = (float)(*tw)[1].num; }
    s.haveAirport = true;
}

std::vector<DelRequest> composeDelivery(const std::vector<Aircraft>& ac) {
    std::vector<DelRequest> out;
    for (const auto& a : ac) {
        if (a.kind != "dep" || a.ga || (a.state != "clr_req" && a.state != "cleared")) continue;
        DelRequest r;
        r.id = a.id; r.cs = a.cs; r.type = a.type; r.wtc = a.wtc; r.dest = a.other; r.destName = a.otherName; r.stand = a.stand;
        r.sid = a.sid; r.cfl = std::to_string(a.cfl > 0 ? a.cfl : 5000); r.squawk = !a.assignedSq.empty() ? a.assignedSq : a.squawk;
        r.cleared = a.state == "cleared"; r.waitingS = r.cleared ? -1 : a.waitingS; r.live = a.human;
        if (!r.cleared) {
            const std::string who = !a.tel.empty() ? a.tel + (a.num.empty() ? "" : " " + a.num) : a.cs;
            r.clearance = who + " cleared to " + (!a.otherName.empty() ? a.otherName : a.other) + " via " + (r.sid.empty() ? std::string("flight planned route") : r.sid + " departure") +
                          ", climb " + r.cfl + " feet, squawk " + r.squawk;
        }
        out.push_back(std::move(r));
    }
    return out;
}

void applyState(State& s, const json::Value& m, const std::string& position) {
    s.rwy = m.s("rwy");
    s.lvp = m.truthy("lvp");
    s.split = m.truthy("split");
    s.atis = json::stripHtml(m.s("atis"));
    s.metar = m.s("metar");
    s.hostNick = m.s("hostNick");
    s.utcMs = (long long)m.n("utc");
    s.haveStopBars = m.at("stopBars") != nullptr;
    s.stopBars = m.truthy("stopBars");
    s.closedRunways.clear();
    for (size_t i = 0; i < m.a("closed").size(); ++i) if (m.a("closed")[i].isNumber()) s.closedRunways.push_back((int)m.a("closed")[i].num);
    const json::Value* claims = m.at("claims");
    if (claims && !s.myId.empty()) { const json::Value* mine = claims->at(s.myId.c_str()); if (mine && mine->isString()) s.myPos = mine->str; }
    s.positions.clear();
    for (const auto& kv : m.o("positions").obj) s.positions[kv.first] = {kv.second.s("name"), kv.second.s("freq"), kv.second.s("cs")};
    const std::string key = !s.myPos.empty() ? s.myPos : (s.split ? position : "ALL");
    s.log.clear();
    const json::Value& lg = m.o("logs").a(key.c_str());
    const size_t n = lg.size();
    for (size_t i = n > kMaxLog ? n - kMaxLog : 0; i < n; ++i) {
        LogEntry e = parseLogEntry(lg[i].s("cls"), lg[i].s("html"));
        if (!e.text.empty() || !e.from.empty()) s.log.push_back(std::move(e));
    }
    std::vector<Aircraft> list;
    const json::Value& ac = m.a("ac");
    bool selStill = false;
    for (size_t i = 0; i < ac.size(); ++i) {
        const json::Value& a = ac[i];
        Aircraft r;
        r.id = (int)a.n("id");
        r.cs = a.s("cs"); r.tel = a.s("tel"); r.num = a.s("num"); r.type = a.s("type"); r.wtc = a.s("wtc"); r.kind = a.s("kind"); r.state = a.s("state");
        r.freq = a.s("freq"); r.squawk = a.s("squawk"); r.assignedSq = a.s("assignedSq");
        r.stand = a.s("stand"); r.rwy = a.s("rwy"); r.sid = a.s("sid"); r.vtype = a.s("vtype");
        r.cfl = (int)a.n("cfl");
        const json::Value& other = a.a("other");
        r.other = other.size() ? other[0].str : "";
        r.otherName = other.size() > 1 ? other[1].str : "";
        r.x = (float)a.a("pos")[0].num; r.y = (float)a.a("pos")[1].num;
        r.lat = s.lat0 + r.y / s.ky; r.lon = s.lon0 + r.x / s.kx;
        r.altFt = (float)a.n("alt"); r.hdg = (float)a.n("hdg"); r.gsKt = (float)(a.n("spd") / 0.514444);
        r.onGround = a.truthy("onGround"); r.veh = a.truthy("veh"); r.human = a.truthy("human"); r.ga = a.truthy("ga");
        r.quiet = a.truthy("quiet"); r.dormant = a.truthy("dormant");
        const json::Value& h = a.o("handoff");
        if (h.isObject() && !h.truthy("accepted")) { r.handoffFrom = h.s("from"); r.handoffTo = h.s("to"); r.handoffIn = r.handoffTo == key; r.handoffAge = (int)h.n("age"); }
        const json::Value& strip = a.o("strip");
        if (strip.isObject()) { r.stripBay = strip.s("bay"); r.stripState = strip.s("state"); }
        r.callingUntilMs = (long long)a.n("callingUntil");
        const json::Value* w = a.at("waiting");
        r.waitingS = w && w->isNumber() ? (int)w->num : -1;
        for (size_t k = 0; k < a.a("warn").size(); ++k) if (a.a("warn")[k].isString()) r.warn.push_back(a.a("warn")[k].str);
        if (r.id == s.selectedId) selStill = true;
        list.push_back(std::move(r));
    }
    s.ac.swap(list);
    if (!selStill) s.selectedId = -1;
    const json::Value* del = m.at("delivery");
    if (del && del->isArray()) {
        s.delivery.clear();
        for (size_t i = 0; i < del->size(); ++i) {
            const json::Value& d = (*del)[i];
            DelRequest r;
            r.id = (int)d.n("id"); r.cs = d.s("cs"); r.type = d.s("type"); r.wtc = d.s("wtc"); r.dest = d.s("dest"); r.destName = d.s("destName");
            r.stand = d.s("stand"); r.sid = d.s("sid"); r.cfl = d.s("cfl"); r.squawk = d.s("squawk"); r.clearance = d.s("clearance"); r.note = d.s("note");
            r.waitingS = (int)d.n("waiting", -1); r.cleared = d.truthy("cleared"); r.live = d.truthy("live");
            s.delivery.push_back(std::move(r));
        }
    } else {
        s.delivery = composeDelivery(s.ac);
    }
}

bool parseAirportLine(const std::string& line, State& s) {
    json::Value m;
    std::string err;
    if (!json::parse(line, &m, &err) || !m.isObject() || m.s("type") != "airport") return false;
    applyAirport(s, m);
    return true;
}

bool parseStateLine(const std::string& line, State& s, const std::string& position) {
    json::Value m;
    std::string err;
    if (!json::parse(line, &m, &err) || !m.isObject() || m.s("type") != "state") return false;
    applyState(s, m, position);
    return true;
}

}  // namespace tower
}  // namespace xr
