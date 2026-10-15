#include "simbrief.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

#include "json.h"
#include "update.h"

namespace xr {
namespace simbrief {

namespace {
std::thread        g_worker;
std::mutex         g_mx;
Ofp                g_result;
bool               g_ready = false;
std::atomic<bool>  g_busy{false};

std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
std::string hhmm(const std::string& epoch) {
    const long long t = atoll(epoch.c_str());
    if (t <= 0) return "";
    const long long s = t % 86400;
    char b[8]; snprintf(b, sizeof b, "%02lld%02lld", s / 3600, (s % 3600) / 60);
    return b;
}
int toInt(const json::Value& v, const char* key) {
    const json::Value* x = v.at(key);
    if (!x) return 0;
    if (x->isNumber()) return (int)x->num;
    return atoi(x->str.c_str());
}
double toNum(const json::Value& v, const char* key) {
    const json::Value* x = v.at(key);
    if (!x) return 0.0;
    if (x->isNumber()) return x->num;
    return atof(x->str.c_str());
}
std::string str(const json::Value& v, const char* key) {
    const json::Value* x = v.at(key);
    if (!x) return "";
    if (x->isString()) return x->str;
    if (x->isNumber()) { char b[32]; snprintf(b, sizeof b, "%g", x->num); return b; }
    return "";
}
}  // namespace

std::string Ofp::summary() const {
    char lvl[16];
    if (cruiseFt >= 6000) snprintf(lvl, sizeof lvl, "FL%03d", cruiseFt / 100); else snprintf(lvl, sizeof lvl, "%d ft", cruiseFt);
    return callsign + " " + acType + "/" + wtc + " " + dep + "-" + dest + " " + lvl + " " + route;
}

Ofp parse(const std::string& text) {
    Ofp o;
    json::Value root;
    std::string err;
    if (!json::parse(text, &root, &err) || !root.isObject()) { o.error = "SimBrief: not a JSON OFP (" + err + ")"; return o; }
    const json::Value& fetch = root.o("fetch");
    const std::string status = str(fetch, "status");
    if (!status.empty() && status != "Success" && status != "success") { o.error = "SimBrief: " + status; return o; }
    const json::Value& g = root.o("general");
    const json::Value& atc = root.o("atc");
    const json::Value& ac = root.o("aircraft");
    const json::Value& orig = root.o("origin");
    const json::Value& dest = root.o("destination");
    const json::Value* altn = root.at("alternate");
    const json::Value& fuel = root.o("fuel");
    const json::Value& w = root.o("weights");
    const json::Value& t = root.o("times");
    const json::Value& params = root.o("params");
    const json::Value& files = root.o("files");
    const json::Value& fms = root.o("fms_downloads");
    o.airline = str(g, "icao_airline"); o.flightNumber = str(g, "flight_number");
    o.callsign = str(atc, "callsign");
    if (o.callsign.empty()) o.callsign = o.airline + o.flightNumber;
    o.acType = str(ac, "icaocode"); if (o.acType.empty()) o.acType = str(ac, "icao_code"); if (o.acType.empty()) o.acType = str(ac, "base_type");
    o.reg = upper(str(ac, "reg")); { std::string r; for (char c : o.reg) if (c != '-') r.push_back(c); o.reg = r; }
    o.equip = str(ac, "equip"); o.selcal = str(ac, "selcal");
    o.fpl = str(atc, "flightplan_text"); o.remarks = str(atc, "section18");
    o.wtc = "M";
    { const size_t i = o.fpl.find("/"); if (i != std::string::npos && i + 1 < o.fpl.size() && i > 0 && o.fpl[i - 1] != '-' ) { const char c = o.fpl[i + 1]; if (c == 'L' || c == 'M' || c == 'H' || c == 'J') o.wtc = std::string(1, c); } }
    o.dep = str(orig, "icao_code"); o.dest = str(dest, "icao_code"); o.depRwy = str(orig, "plan_rwy"); o.destRwy = str(dest, "plan_rwy");
    if (altn) { const json::Value& a = altn->isArray() ? (*altn)[0] : *altn; o.altn = str(a, "icao_code"); }
    o.sid = str(g, "sid_ident"); o.star = str(g, "star_ident");
    o.route = str(g, "route"); o.routeAtc = str(atc, "route");
    o.cruiseFt = toInt(g, "initial_altitude"); o.tas = toInt(g, "cruise_tas"); o.mach = toNum(g, "cruise_mach");
    o.eobt = hhmm(str(t, "sched_out")); o.etd = hhmm(str(t, "est_off")); o.eta = hhmm(str(t, "est_on"));
    o.blockFuel = toInt(fuel, "plan_ramp"); o.tripFuel = toInt(fuel, "enroute_burn");
    o.zfw = toInt(w, "est_zfw"); o.tow = toInt(w, "est_tow"); o.ldw = toInt(w, "est_ldw");
    o.units = str(params, "units").find("lb") != std::string::npos ? "lb" : "kg";
    const std::string dir = str(files, "directory").empty() ? str(fms, "directory") : str(files, "directory");
    const std::string pdf = str(files.o("pdf"), "link");
    if (!pdf.empty()) o.pdfUrl = pdf.rfind("http", 0) == 0 ? pdf : dir + pdf;
    const std::string xpe = str(fms.o("xpe"), "link");
    if (!xpe.empty()) o.fmsUrl = xpe.rfind("http", 0) == 0 ? xpe : dir + xpe;
    const json::Value& navlog = root.o("navlog");
    const json::Value& fixes = navlog.a("fix");
    for (size_t i = 0; i < fixes.size(); ++i) {
        const json::Value& f = fixes[i];
        Fix x; x.ident = str(f, "ident"); x.lat = toNum(f, "pos_lat"); x.lon = toNum(f, "pos_long"); x.altFt = toInt(f, "altitude_feet");
        x.airway = str(f, "via_airway"); x.sidStar = str(f, "is_sid_star") == "1"; x.type = str(f, "type");
        if (!x.ident.empty()) o.fixes.push_back(x);
    }
    o.ok = !o.dep.empty() && !o.dest.empty();
    if (!o.ok) o.error = "SimBrief: the OFP has no origin/destination";
    return o;
}

std::string fetchUrl(const std::string& user) {
    std::string u;
    for (char c : user) if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') u.push_back(c);
    if (u.empty() || u.size() > 64) return "";
    bool digits = true;
    for (char c : u) if (!isdigit((unsigned char)c)) digits = false;
    const char* base = getenv("XRADIO_SIMBRIEF_URL");   // the tests point this at tools/fake_simbrief.py
    return std::string(base && *base ? base : "https://www.simbrief.com/api/xml.fetcher.php") + "?" + (digits ? "userid=" : "username=") + u + "&json=1";
}

std::string explain(const std::string& httpError, const std::string& body) {
    if (httpError.find("400") != std::string::npos) return "SimBrief: unknown username / pilot ID, or no flight has been planned yet (HTTP 400)";
    if (httpError.find("429") != std::string::npos) return "SimBrief: too many requests, wait a moment (HTTP 429)";
    const size_t a = body.find("<status>"), b = body.find("</status>");
    if (a != std::string::npos && b != std::string::npos && b > a) return "SimBrief: " + body.substr(a + 8, b - a - 8);
    json::Value v;
    if (!body.empty() && body[0] == '{' && json::parse(body, &v)) {
        const json::Value* st = v.o("fetch").at("status");
        if (st && st->isString() && !st->str.empty()) return "SimBrief: " + st->str;
    }
    return "SimBrief not reachable: " + httpError;
}

void fetchAsync(const std::string& user) {
    if (g_busy.load()) return;
    const std::string url = fetchUrl(user);
    if (g_worker.joinable()) g_worker.join();
    if (url.empty()) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_result = Ofp(); g_result.error = "SimBrief: enter your SimBrief username or pilot ID (Settings > Flight plan)"; g_ready = true;
        return;
    }
    g_busy.store(true);
    g_worker = std::thread([url] {
        std::string body, err;
        Ofp o;
        if (update::httpGet(url, &body, &err)) o = parse(body);
        else { o.error = explain(err, body); }
        std::lock_guard<std::mutex> lk(g_mx);
        g_result = o; g_ready = true;
        g_busy.store(false);
    });
}

bool poll(Ofp* out) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_ready) return false;
    g_ready = false;
    if (out) *out = g_result;
    return true;
}

bool busy() { return g_busy.load(); }
void shutdown() { if (g_worker.joinable()) g_worker.join(); }

std::string toFms(const Ofp& o) {
    // X-Plane 11/12 flight plan: header, airports, then one row per waypoint (type, ident, via/special, alt, lat, lon)
    std::string s = "I\n1100 Version\nCYCLE 2410\n";
    s += "ADEP " + o.dep + "\n";
    if (!o.depRwy.empty()) s += "DEPRWY RW" + o.depRwy + "\n";
    s += "ADES " + o.dest + "\n";
    if (!o.destRwy.empty()) s += "DESRWY RW" + o.destRwy + "\n";
    std::vector<const Fix*> pts;
    for (const Fix& f : o.fixes) if (f.type != "ltlg" && f.ident != "TOC" && f.ident != "TOD") pts.push_back(&f);
    s += "NUMENR " + std::to_string(pts.size()) + "\n";
    for (size_t i = 0; i < pts.size(); ++i) {
        const Fix& f = *pts[i];
        const int type = f.type == "apt" ? 1 : f.type == "vor" ? 3 : f.type == "ndb" ? 2 : f.type == "wpt" ? 11 : 28;
        const char* via = i == 0 ? "ADEP" : i + 1 == pts.size() ? "ADES" : (f.airway.empty() || f.airway == "DCT" || f.sidStar ? "DRCT" : f.airway.c_str());
        char line[160];
        snprintf(line, sizeof line, "%d %s %s %d %.6f %.6f\n", type, f.ident.c_str(), via, f.altFt, f.lat, f.lon);
        s += line;
    }
    return s;
}

std::string writeFms(const Ofp& o, const std::string& xplaneRoot, bool overwrite, std::string* pathOut) {
    if (!o.ok) return "no OFP to write";
    if (xplaneRoot.empty()) return "X-Plane folder unknown";
    const std::string dir = xplaneRoot + "/Output/FMS plans";
    const std::string path = dir + "/" + o.dep + o.dest + ".fms";
    if (pathOut) *pathOut = path;
    {
        std::ifstream probe(path);
        if (probe.good() && !overwrite) return "exists: " + path + " -- press again to overwrite";
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) return "cannot write " + path + " (does Output/FMS plans exist?)";
    f << toFms(o);
    return "written: " + path + " -- load it in the FMS as " + o.dep + o.dest;
}

}  // namespace simbrief
}  // namespace xr
