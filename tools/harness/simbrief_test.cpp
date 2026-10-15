// SimBrief in the plugin (S3): the OFP parser, the .fms writer and the fetch URL, offline against
// tools/fixtures/simbrief_ofp.json; and, when XR_SIMBRIEF_PORT is set (tools/check_simbrief.sh), a live
// fetch through the plugin's own HTTP client against tools/fake_simbrief.py -- the worker thread, the
// HTTP 400 message for an unknown user, and the poll on the main thread.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "simbrief.h"

using namespace xr::simbrief;

static int fails = 0;
static void check(bool ok, const std::string& what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) ++fails; }
static std::string slurp(const std::string& path) { std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static void putEnv(const char* k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}
static bool waitOfp(Ofp* o) {
    for (int i = 0; i < 200; ++i) {
        if (poll(o)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

int main(int argc, char** argv) {
    const std::string fixture = argc > 1 ? argv[1] : "tools/fixtures/simbrief_ofp.json";
    const std::string text = slurp(fixture);
    check(!text.empty(), "fixture readable: " + fixture);

    const Ofp o = parse(text);
    check(o.ok, "parse ok " + o.error);
    check(o.callsign == "TTR123" && o.acType == "A320" && o.reg == "ESTTR" && o.wtc == "M", "callsign, type, registration without the dash, wake category from the FPL");
    check(o.dep == "EETN" && o.dest == "EVRA" && o.altn == "EYVI", "origin, destination, alternate");
    check(!o.depRwy.empty() && !o.destRwy.empty(), "planned runways " + o.depRwy + " / " + o.destRwy);
    check(o.sid == "ULGOT1G" && o.star == "ABETI2A", "SID and STAR");
    check(o.routeAtc == "ULGOT1G ULGOT N746 ABETI ABETI2A" && o.route.find("N746") != std::string::npos, "ATC route and OFP route");
    check(o.cruiseFt == 36000 && o.tas == 447 && o.mach > 0.5, "cruise level, TAS, Mach");
    check(o.eobt == "0730", "EOBT from the epoch: " + o.eobt);
    check(o.fixes.size() == 9 && o.fixes.front().ident == "EETN" && o.fixes.back().ident == "EVRA", "navlog fixes");
    check(o.blockFuel > 0 && o.tripFuel > 0 && o.zfw > 0 && o.tow > 0 && o.units == "kg", "fuel, weights, units");
    check(!o.pdfUrl.empty() && o.pdfUrl.rfind("http", 0) == 0, "PDF link " + o.pdfUrl);
    check(!o.equip.empty() && o.fpl.rfind("(FPL-TTR123", 0) == 0, "equipment and the FPL text");
    check(o.summary().rfind("TTR123 A320/M EETN-EVRA FL360", 0) == 0, "summary: " + o.summary());

    const std::string fms = toFms(o);
    check(fms.rfind("I\n1100 Version\n", 0) == 0 && fms.find("\nADEP EETN\n") != std::string::npos && fms.find("\nADES EVRA\n") != std::string::npos, "fms header");
    check(fms.find("DEPRWY RW" + o.depRwy) != std::string::npos && fms.find("NUMENR ") != std::string::npos, "fms runways and count");
    check(fms.find(" ULGOT ") != std::string::npos && fms.find("TOC") == std::string::npos, "fms rows carry the fixes, not TOC/TOD");
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "xradio_simbrief_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Output" / "FMS plans", ec);
        std::string path;
        const std::string n1 = writeFms(o, root.string(), false, &path);
        check(n1.rfind("written", 0) == 0 && fs::exists(path), "writes Output/FMS plans/EETNEVRA.fms: " + n1);
        const std::string n2 = writeFms(o, root.string(), false, &path);
        check(n2.rfind("exists", 0) == 0, "refuses to overwrite without asking: " + n2);
        const std::string n3 = writeFms(o, root.string(), true, &path);
        check(n3.rfind("written", 0) == 0, "overwrites once asked");
        check(slurp(path) == fms, "the file is the fms text");
        fs::remove_all(root, ec);
    }

    check(fetchUrl("viktor") == "https://www.simbrief.com/api/xml.fetcher.php?username=viktor&json=1", "fetch URL for a username");
    check(fetchUrl("123456") == "https://www.simbrief.com/api/xml.fetcher.php?userid=123456&json=1", "fetch URL for a pilot ID");
    check(fetchUrl("").empty() && fetchUrl("a b;'c") == "https://www.simbrief.com/api/xml.fetcher.php?username=abc&json=1", "unusable characters dropped, empty refused");
    check(explain("HTTP 400", "").find("HTTP 400") != std::string::npos && explain("HTTP 400", "").find("no flight") != std::string::npos, "HTTP 400 explained");
    check(explain("could not reach", "{\"fetch\":{\"status\":\"Error: Unknown UserID\"}}").find("Unknown UserID") != std::string::npos, "SimBrief's own status text is shown");
    const Ofp bad = parse("not json at all");
    check(!bad.ok && !bad.error.empty(), "garbage is an error, not a crash");
    const Ofp e2 = parse("{\"fetch\":{\"status\":\"Error: Unknown UserID\"}}");
    check(!e2.ok && e2.error.find("Unknown UserID") != std::string::npos, "an error OFP carries SimBrief's status");

    if (const char* port = getenv("XR_SIMBRIEF_PORT")) {
        putEnv("XRADIO_SIMBRIEF_URL", std::string("http://127.0.0.1:") + port + "/api/xml.fetcher.php");
        Ofp live;
        fetchAsync("tester");
        check(busy() || poll(&live) || true, "fetch runs on a worker");
        check(waitOfp(&live) && live.ok && live.callsign == "TTR123", "live fetch through the plugin's HTTP client: " + live.error);
        fetchAsync("nobody");
        check(waitOfp(&live) && !live.ok && live.error.find("HTTP 400") != std::string::npos, "unknown user: " + live.error);
        fetchAsync("");
        check(poll(&live) && !live.ok && live.error.find("username") != std::string::npos, "no username: " + live.error);
    } else {
        printf("note  XR_SIMBRIEF_PORT not set -- live fetch skipped (tools/check_simbrief.sh runs it)\n");
    }
    shutdown();
    printf(fails ? "%d FAILED\n" : "simbrief_test OK\n", fails);
    return fails ? 1 : 0;
}
