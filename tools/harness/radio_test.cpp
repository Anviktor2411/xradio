// The radio model on its own (radio.h, shadow.h; docs/radio.md): the horizon
// from heights above the ground, a station's service volume, the link budget,
// ducting, the knife-edge shadow of a ridge on a fixture height profile, and
// the path sampler keeping to its probe budget. No X-Plane, no audio: the
// capture effect is tested where the audio is, in voice_test.
//   ./radio_test [tools/fixtures/radio_profile_ridge.txt]
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "radio.h"
#include "shadow.h"

using namespace xr;
using radio::Node;

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}
static std::string num(double v) { char b[48]; snprintf(b, sizeof b, "%.4g", v); return b; }

static Node aircraft(double lat, double lon, double altMslM, double aglM) {
    Node n;
    n.lat = lat; n.lon = lon; n.altMslM = altMslM;
    n.heightFt = radio::aircraftHeightFt(aglM);
    return n;
}
static Node station(double lat, double lon, double altMslM, uint8_t kind) {
    const radio::StationDefaults d = radio::stationDefaults(kind);
    Node n;
    n.lat = lat; n.lon = lon; n.altMslM = altMslM;
    n.heightFt = d.antennaM * radio::kFtPerM;
    n.atc = true; n.rangeNm = d.rangeNm; n.ceilingFt = d.ceilingFt;
    return n;
}

static std::vector<radio::ProfilePoint> loadProfile(const std::string& path) {
    std::vector<radio::ProfilePoint> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        radio::ProfilePoint p;
        int wet = 0;
        if (s >> p.km >> p.groundM >> wet) { p.wet = wet != 0; p.known = true; out.push_back(p); }
    }
    return out;
}

int main(int argc, char** argv) {
    const std::string fixture = argc > 1 ? argv[1] : "tools/fixtures/radio_profile_ridge.txt";

    printf("heights above the ground\n");
    const double ground = radio::aircraftHeightFt(0.0);
    check("two aerials 3 m up see 7.7 nm (Innsbruck's apron, whatever its 1900 ft)",
          std::fabs(radio::horizonNm(ground, ground) - 7.72) < 0.01, num(radio::horizonNm(ground, ground)));
    check("never less than the 5 nm floor", radio::horizonNm(0.0, 0.0) == radio::kRangeFloorNm);
    check("a v5 report: on the ground 0 above it, in the air its altitude",
          radio::effectiveAglM(false, 0.0, true, 581.0) == 0.0 && radio::effectiveAglM(false, 0.0, false, 914.4) == 914.4);
    const Node lowiA = aircraft(47.2602, 11.3439, 581.0, 0.0);
    check("2.4 nm across the airport: clean",
          radio::link(lowiA, aircraft(47.3002, 11.3439, 581.0, 0.0), 120100, false).quality == 1.0);
    check("10 nm down the valley: nothing",
          radio::link(lowiA, aircraft(47.4269, 11.3439, 581.0, 0.0), 120100, false).quality == 0.0);

    printf("\nservice volumes\n");
    const Node twr = station(59.4133, 24.8328, 70.0, ATC_TWR);
    const radio::Link fin = radio::link(twr, aircraft(59.4133, 24.5054, 914.4, 873.0), 118100, false);
    check("the tower and a 10 nm final at 3000 ft: clean", fin.quality == 1.0, num(fin.quality));
    const Node rim = aircraft(59.7466, 24.8328, 40.0, 0.0);      // 20 nm north, on the ground
    check("on the ground 20 nm out: past the horizon (18.9 nm)",
          radio::horizonFor(twr, rim, false) < 20.0 && radio::serviceRangeFor(twr, rim) == 25.0);
    check("...and clean anyway, inside the tower's 25 nm", radio::link(twr, rim, 118100, false).quality == 1.0);
    check("the service volume is the station's: two aircraft there do not get it",
          radio::link(rim, aircraft(59.4133, 24.8328, 40.0, 0.0), 118100, false).quality == 0.0);
    const Node above = aircraft(59.7466, 24.8328, 3000.0, 2960.0);
    check("above its 4000 ft ceiling only the horizon counts", radio::serviceRangeFor(twr, above) == 0.0 &&
          !radio::inServiceVolume(twr, above) && radio::inServiceVolume(twr, rim));
    const Node ctr = station(59.4133, 24.8328, 70.0, ATC_CTR);
    const radio::Link ctr240 = radio::link(ctr, aircraft(63.4133, 24.8328, 1000.0, 980.0), 132800, false);
    check("a centre covers its area, 240 nm out at 3000 ft, fading only at its edge",
          ctr240.quality > 0.0 && ctr240.coverageNm == 250.0, num(ctr240.quality));

    printf("\nthe link budget\n");
    const Node hi = aircraft(62.7466, 24.8328, 10668.0, 10600.0);
    const radio::Link up = radio::link(hi, twr, 132800, false), down = radio::link(twr, hi, 132800, false);
    check("FL350 and a ground station 200 nm apart: readable, noisy",
          up.quality > 0.2 && up.quality < 0.9 && up.coverageNm > 240.0, num(up.quality) + " of " + num(up.coverageNm));
    check("25 W down reaches better than 10 W up", down.quality > up.quality && down.levelDbm > up.levelDbm);
    check("260 nm: past the horizon, nothing",
          radio::link(aircraft(63.7466, 24.8328, 10668.0, 10600.0), twr, 132800, false).quality == 0.0);
    double last = 2.0;
    bool falling = true;
    for (int nm = 1; nm <= 250; nm += 3) {
        const double q = radio::link(aircraft(57.0, 27.0, 3048.0, 3048.0), aircraft(57.0 + nm / 60.0, 27.0, 3048.0, 3048.0),
                                     122800, false).quality;
        falling = falling && q <= last + 1e-12;
        last = q;
    }
    check("further is never better", falling);
    check("free space path loss: 32.45 + 20 log f + 20 log d", std::fabs(radio::fsplDb(125.0, 10.0) - 94.39) < 0.01,
          num(radio::fsplDb(125.0, 10.0)));
    check("a frequency outside the airband cannot break the sums",
          std::isfinite(radio::link(lowiA, aircraft(47.3002, 11.3439, 581.0, 0.0), 0, false).levelDbm));

    printf("\nducting\n");
    const Node seaA = aircraft(59.60, 22.00, 152.4, 152.4), seaB = aircraft(59.60, 24.6350, 152.4, 152.4);
    check("80 nm at 500 ft over the sea: past the horizon without a duct, in it with one",
          radio::link(seaA, seaB, 121500, false).quality == 0.0 && radio::link(seaA, seaB, 121500, true).quality > 0.5);
    check("a duct is low: FL350 is not in it",
          radio::horizonFor(hi, aircraft(59.0, 24.0, 10668.0, 10600.0), true) ==
          radio::horizonFor(hi, aircraft(59.0, 24.0, 10668.0, 10600.0), false));

    printf("\nterrain (%s)\n", fixture.c_str());
    std::vector<radio::ProfilePoint> ridge = loadProfile(fixture);
    check("the fixture profile loads", ridge.size() == 39, std::to_string(ridge.size()) + " points");
    const radio::Shadow deep = radio::shadowOf(ridge.data(), (int)ridge.size(), 40.0, 903.0, 903.0, 120100, false);
    check("a ridge 1500 m above the line half way: deeply blocked, nothing gets through",
          deep.blocked && deep.nu > radio::kDeepNu && deep.cap == 0.0, "nu " + num(deep.nu));
    auto lowered = [&](double peak) {
        std::vector<radio::ProfilePoint> p = ridge;
        for (auto& q : p) q.groundM = 600.0 + (q.groundM - 600.0) * (peak - 600.0) / 1800.0;
        return p;
    };
    std::vector<radio::ProfilePoint> hill = lowered(1000.0);
    const radio::Shadow behind = radio::shadowOf(hill.data(), (int)hill.size(), 40.0, 903.0, 903.0, 120100, false);
    check("a ridge 100 m above the line: shadowed -- some 15 dB lost, weak and noisy at best",
          behind.blocked && behind.lossDb > 10.0 && behind.lossDb < 20.0 && behind.cap > 0.2 && behind.cap < 0.45,
          "loss " + num(behind.lossDb) + " dB, cap " + num(behind.cap));
    std::vector<radio::ProfilePoint> low = lowered(800.0);
    const radio::Shadow clear = radio::shadowOf(low.data(), (int)low.size(), 40.0, 903.0, 903.0, 120100, false);
    check("a ridge just under the line: clear (a fraction of a dB)", !clear.blocked && clear.cap == 1.0 &&
          clear.lossDb < 1.0, "nu " + num(clear.nu));
    const radio::Shadow overIt = radio::shadowOf(hill.data(), (int)hill.size(), 40.0, 903.0, 2500.0, 120100, false);
    check("the same hill from 2500 m: seen over", !overIt.blocked && overIt.cap == 1.0);
    // the earth's bulge alone, over flat sea: blocks two 500 ft aerials 80 nm apart
    std::vector<radio::ProfilePoint> sea;
    for (int k = 1; k < 148; ++k) { radio::ProfilePoint p; p.km = k; p.groundM = 0.0; p.wet = true; p.known = true; sea.push_back(p); }
    const radio::Shadow bulge = radio::shadowOf(sea.data(), (int)sea.size(), 148.2, 155.0, 155.0, 121500, false);
    const radio::Shadow ducted = radio::shadowOf(sea.data(), (int)sea.size(), 148.2, 155.0, 155.0, 121500, true);
    check("over the sea the bulge itself is in the way 80 nm out, and a duct takes it away",
          bulge.blocked && !ducted.blocked && ducted.wetFraction == 1.0);
    std::vector<radio::ProfilePoint> unknown = ridge;
    for (auto& p : unknown) p.known = false;
    check("where no scenery is loaded nothing is assumed in the way",
          !radio::shadowOf(unknown.data(), (int)unknown.size(), 40.0, 903.0, 903.0, 120100, false).blocked);

    printf("\nthe path sampler\n");
    {
        double lat = 0, lon = 0;
        shadow::along(0.0, 0.0, 0.0, 10.0, 0.5, &lat, &lon);
        check("half way along the equator from 0 to 10 E is 5 E", std::fabs(lat) < 1e-9 && std::fabs(lon - 5.0) < 1e-9);
        shadow::along(59.0, 24.0, 59.0, 24.0, 0.3, &lat, &lon);
        check("and a path of no length stays put", lat == 59.0 && lon == 24.0);
    }
    // a made-up landscape: flat at 600 m, a ridge (2400 m) along 47.5 N
    int calls = 0;
    shadow::Tracker t([&](double la, double, double* g, bool* wet) {
        ++calls;
        *g = std::fabs(la - 47.5) < 0.03 ? 2400.0 : 600.0;
        *wet = false;
        return true;
    });
    const shadow::Endpoint me{47.30, 11.34, 903.0}, across{47.70, 11.34, 903.0}, along1{47.30, 11.70, 903.0};
    t.want(1, me, across, 120100, true, 0.0);
    t.want(2, me, along1, 120100, false, 0.0);
    bool known = true;
    t.result(1, &known);
    check("nothing known before any probing", !known);
    int maxTick = 0, ticks = 0;
    for (; ticks < 40; ++ticks) {
        const int n = t.tick(0.1 * ticks, 16, 1000.0);
        if (n > maxTick) maxTick = n;
        bool k1 = false, k2 = false;
        t.result(1, &k1); t.result(2, &k2);
        if (k1 && k2) break;
    }
    check("never more probes in a frame than allowed", maxTick <= 16, std::to_string(maxTick));
    bool k1 = false, k2 = false;
    const radio::Shadow s1 = t.result(1, &k1), s2 = t.result(2, &k2);
    check("both paths are answered within a few frames", k1 && k2, std::to_string(ticks) + " frames");
    check("the one across the ridge is blocked, the one along the valley is clear",
          s1.blocked && s1.cap == 0.0 && !s2.blocked, "nu " + num(s1.nu) + " / " + num(s2.nu));
    const int before = calls;
    for (int k = 0; k < 20; ++k) {               // ten seconds, nobody moves
        t.want(1, me, across, 120100, true, 10.0 + k);
        t.want(2, me, along1, 120100, false, 10.0 + k);
        t.tick(10.0 + k, 16, 1000.0);
    }
    check("while nobody moves, nothing is probed again", calls == before, std::to_string(calls - before) + " probes");
    t.want(1, me, across, 132800, true, 31.0);
    t.result(1, &k1);
    check("another frequency is answered from the same terrain at once", calls == before && k1);
    const shadow::Endpoint moved{47.30, 11.40, 903.0};      // 4.5 km east
    t.want(1, moved, across, 132800, true, 32.0);
    t.tick(32.0, 16, 1000.0);
    check("moving a few kilometres probes the path again", calls > before);
    t.forgetIdle(100.0, 30.0);
    check("paths nobody asks about are forgotten", t.paths() == 0);
    shadow::Tracker slow([&](double, double, double* g, bool* wet) {
        volatile double x = 0;
        for (int i = 0; i < 200000; ++i) x = x + std::sqrt((double)i);
        *g = 0.0; *wet = false;
        return true;
    });
    slow.want(9, me, {49.30, 11.34, 903.0}, 120100, true, 0.0);
    const int got = slow.tick(0.0, 1000, 0.5);
    check("a slow probe makes it stop at its time budget, not its probe budget", got > 0 && got < 1000,
          std::to_string(got) + " probes");

    printf(failures ? "\n%d FAILED\n" : "\nradio_test OK\n", failures);
    return failures ? 1 : 0;
}
