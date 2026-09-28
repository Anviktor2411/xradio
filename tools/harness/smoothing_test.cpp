// Reproduces "the models stutter when they move" and proves the fix.
//
// A simulated aircraft flies a turn at 100 kt. Its reports pass through a
// relay that samples on its own 5 Hz clock -- so some reports are forwarded
// twice and some are skipped -- and then over a jittery network. We draw at
// 60 fps and check that the rendered motion never runs backwards, never jumps,
// and tracks the true path. The old dead-reckoning approach is run through
// the same scenario to show the test really does catch the bug.
#include "smoothing.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "mathconst.h"

using xr::PoseSample;
using xr::Pose;
using xr::Smoother;

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

// --- the true flight path --------------------------------------------------
struct Truth {
    double lat0 = 57.85, lon0 = 27.02;
    double speedMs = 51.44;          // 100 kt
    double turnRateDeg = 3.0;        // standard rate
    double alt0Ft = 3000, vsFps = 5; // gentle climb

    PoseSample at(double t) const {
        // integrate the arc analytically: heading = t * rate
        const double w = turnRateDeg * xr::kPi / 180.0;
        const double r = speedMs / w;
        const double north = r * std::sin(w * t);
        const double east  = r * (1.0 - std::cos(w * t));
        PoseSample s;
        s.t = t;
        s.lat = lat0 + north / 111320.0;
        s.lon = lon0 + east / (111320.0 * std::cos(lat0 * xr::kPi / 180.0));
        s.altFt = (float)(alt0Ft + vsFps * t);
        s.heading = (float)std::fmod(turnRateDeg * t, 360.0);
        s.track = s.heading;             // no wind in this scenario
        s.gsKt = (float)(speedMs / 0.514444);
        s.vsFps = (float)vsFps;
        s.pitch = 3.f;
        s.roll = 15.f;
        return s;
    }
};

static double metres(const Truth& tr, double lat1, double lon1, double lat2, double lon2) {
    const double dy = (lat2 - lat1) * 111320.0;
    const double dx = (lon2 - lon1) * 111320.0 * std::cos(tr.lat0 * xr::kPi / 180.0);
    return std::sqrt(dx * dx + dy * dy);
}

// Signed progress along the true direction of travel, in metres.
static double along(const Truth& tr, const Pose& from, const Pose& to, double trackDeg) {
    const double dy = (to.lat - from.lat) * 111320.0;
    const double dx = (to.lon - from.lon) * 111320.0 * std::cos(tr.lat0 * xr::kPi / 180.0);
    const double tr_ = trackDeg * xr::kPi / 180.0;
    return dx * std::sin(tr_) + dy * std::cos(tr_);
}

// --- the relay + network, producing (arrivalTime, rawMs, sample) events ----
struct Delivery { double arrive; uint32_t rawMs; PoseSample s; };

static std::vector<Delivery> simulateDelivery(const Truth& tr, double seconds, unsigned seed,
                                              uint32_t clockBase = 0) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(-0.04, 0.04);

    // sender reports every 200 ms (with X-Plane's own slight irregularity)
    std::vector<PoseSample> reports;
    std::vector<double> reportT;
    for (double t = 0; t < seconds; t += 0.2 + (rng() % 30) / 1000.0) {
        reports.push_back(tr.at(t));
        reportT.push_back(t);
    }

    // relay: every 200 ms, on its own phase, forwards the newest report it has.
    std::vector<Delivery> out;
    for (double rt = 0.07; rt < seconds; rt += 0.2) {
        int newest = -1;
        for (size_t i = 0; i < reportT.size(); ++i) {
            if (reportT[i] + 0.05 <= rt) newest = (int)i;   // 50 ms sender->relay
        }
        if (newest < 0) continue;
        Delivery d;
        d.s = reports[(size_t)newest];
        d.rawMs = clockBase + (uint32_t)std::llround(reportT[(size_t)newest] * 1000.0);
        d.arrive = rt + 0.08 + jitter(rng);                  // relay->receiver
        out.push_back(d);
    }
    return out;
}

// The approach being replaced: jump to each report, dead-reckon until the next.
struct NaiveDeadReckoning {
    PoseSample last; bool have = false; double since = 0;
    void push(const PoseSample& s) { last = s; have = false; since = 0; have = true; }
    double shown() const { return last.t + since; }
    Pose sample(double dt) {
        since += dt;
        Pose p; p.lat = last.lat; p.lon = last.lon; p.altFt = last.altFt; p.heading = last.heading;
        const double distM = last.gsKt * 0.514444 * since;
        const double tr = last.track * xr::kPi / 180.0;
        p.lat += distM * std::cos(tr) / 111320.0;
        p.lon += distM * std::sin(tr) / (111320.0 * std::cos(last.lat * xr::kPi / 180.0));
        return p;
    }
};

static double shownTime(const NaiveDeadReckoning& n, double) { return n.shown(); }

struct Result { int backwards = 0; int jumps = 0; int headingJumps = 0; double maxErrM = 0; double meanErrM = 0; int frames = 0; };

// Which instant of the true path a renderer is showing: the smoother knows
// (its render clock); the naive one shows the newest report's time plus
// however long it has dead-reckoned since.
static double shownTime(const Smoother& s, double) { return s.renderTime(); }
struct NaiveDeadReckoning;
static double shownTime(const NaiveDeadReckoning& n, double now);

template <class Renderer, class Push>
static Result run(const Truth& tr, const std::vector<Delivery>& deliveries, Renderer& r, Push push,
                  double seconds) {
    Result res;
    const double fps = 60.0, dt = 1.0 / fps;
    size_t next = 0;
    Pose prev; bool havePrev = false;
    double sumErr = 0;
    for (double now = 0; now < seconds; now += dt) {
        while (next < deliveries.size() && deliveries[next].arrive <= now) {
            push(r, deliveries[next]);
            ++next;
        }
        if (next == 0) continue;                       // nothing received yet
        Pose p = r.sample(dt);
        if (havePrev) {
            const double truthT = shownTime(r, now);
            const PoseSample truth = tr.at(truthT > 0 ? truthT : 0);
            const double step = along(tr, prev, p, truth.track);
            const double dist = metres(tr, prev.lat, prev.lon, p.lat, p.lon);
            if (step < -0.05) ++res.backwards;         // moved against the direction of travel
            if (dist > 2.5) ++res.jumps;                // 60 fps at 100 kt is 0.86 m per frame
            float dh = std::fabs(p.heading - prev.heading);
            if (dh > 180.f) dh = 360.f - dh;
            if (dh > 1.5f) ++res.headingJumps;
            if (now > 1.5) {                            // after the buffer has filled
                const double err = metres(tr, truth.lat, truth.lon, p.lat, p.lon);
                if (err > res.maxErrM) res.maxErrM = err;
                sumErr += err;
                ++res.frames;
            }
        }
        prev = p; havePrev = true;
    }
    res.meanErrM = res.frames ? sumErr / res.frames : 0;
    return res;
}

int main() {
    Truth tr;
    const double seconds = 20.0;

    printf("\nthe scenario reproduces the stutter with the old approach\n");
    {
        auto deliveries = simulateDelivery(tr, seconds, 1);
        int dup = 0;
        for (size_t i = 1; i < deliveries.size(); ++i) if (deliveries[i].rawMs == deliveries[i - 1].rawMs) ++dup;
        check("relay forwards some reports twice", dup > 5, std::to_string(dup) + " duplicates");

        NaiveDeadReckoning naive;
        auto res = run(tr, deliveries, naive,
                       [](NaiveDeadReckoning& n, const Delivery& d) { n.push(d.s); },
                       seconds);
        check("naive dead reckoning moves backwards on duplicates", res.backwards > 10,
              std::to_string(res.backwards) + " backward steps");
    }

    printf("\nsmoother under the same delivery\n");
    for (unsigned seed = 1; seed <= 3; ++seed) {
        auto deliveries = simulateDelivery(tr, seconds, seed);
        Smoother sm;
        auto res = run(tr, deliveries, sm,
                       [](Smoother& s, const Delivery& d) { s.push(d.rawMs, d.s); },
                       seconds);
        const std::string tag = " (seed " + std::to_string(seed) + ")";
        check("never moves backwards" + tag, res.backwards == 0, std::to_string(res.backwards));
        check("never jumps" + tag, res.jumps == 0, std::to_string(res.jumps) + " jumps");
        check("heading is continuous" + tag, res.headingJumps == 0, std::to_string(res.headingJumps));
        check("tracks the true path within 1 m" + tag, res.maxErrM < 1.0,
              "max " + std::to_string(res.maxErrM) + " m, mean " + std::to_string(res.meanErrM) + " m");
    }

    printf("\nedge cases\n");
    {
        // sender clock wrapping past 2^32 ms mid-flight
        auto deliveries = simulateDelivery(tr, seconds, 4, 0xFFFFFFFFu - 5000u);
        Smoother sm;
        auto res = run(tr, deliveries, sm,
                       [](Smoother& s, const Delivery& d) { s.push(d.rawMs, d.s); },
                       seconds);
        check("survives the sender's 32-bit clock wrapping", res.backwards == 0 && res.jumps == 0,
              std::to_string(res.backwards) + " back, " + std::to_string(res.jumps) + " jumps");
    }
    {
        // the stream stops: extrapolate for a bounded time, then hold still
        Smoother sm;
        for (int i = 0; i < 10; ++i) sm.push((uint32_t)(i * 200), tr.at(i * 0.2));
        Pose a = sm.sample(0.0);
        for (int f = 0; f < 60; ++f) sm.sample(1.0 / 60);          // 1 s: through the buffer, then extrapolating
        Pose b = sm.sample(0.0);
        for (int f = 0; f < 180; ++f) sm.sample(1.0 / 60);         // 3 s more: must have stopped
        Pose c = sm.sample(0.0);
        Pose d = sm.sample(1.0 / 60);
        check("keeps moving when reports stop", metres(tr, a.lat, a.lon, b.lat, b.lon) > 20.0);
        check("holds still after the extrapolation limit",
              metres(tr, c.lat, c.lon, d.lat, d.lon) < 0.01);
        check("extrapolation is bounded (did not fly off)",
              metres(tr, a.lat, a.lon, c.lat, c.lon) < 51.44 * (1.0 + Smoother::kPlayoutS + 1.2));
    }
    {
        // resume after a long gap: no wild interpolation across the gap
        Smoother sm;
        for (int i = 0; i < 5; ++i) sm.push((uint32_t)(i * 200), tr.at(i * 0.2));
        for (int f = 0; f < 120; ++f) sm.sample(1.0 / 60);
        for (int i = 0; i < 5; ++i) sm.push((uint32_t)(30000 + i * 200), tr.at(30.0 + i * 0.2));
        sm.sample(0.0);
        Pose p = sm.sample(1.0 / 60);
        PoseSample truth = tr.at(30.0 + 0.8 - Smoother::kPlayoutS);
        check("snaps to the new position after a long gap",
              metres(tr, truth.lat, truth.lon, p.lat, p.lon) < 60.0,
              std::to_string(metres(tr, truth.lat, truth.lon, p.lat, p.lon)) + " m off");
    }
    {
        Smoother sm;
        check("empty smoother is harmless", sm.empty());
        Pose p = sm.sample(0.5);
        check("sampling an empty smoother returns a default pose", p.lat == 0 && p.lon == 0);
    }

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all smoothing tests passed\n");
    return 0;
}
