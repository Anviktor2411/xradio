// Terrain between two radios (docs/radio.md). The server routes by the horizon
// alone -- it has no scenery -- so the plugin, which has it, is the one that
// finds the mountain in the way: it samples the straight path between this
// aircraft and each station it may hear, a kilometre a probe, and radio.h's
// shadowOf() turns the profile into a loss and a cap on the signal.
//
// Probing is the expensive part, so it is rationed: a few probes a frame, one
// path at a time, the stations that are talking (or tuned) first, and an
// answer is kept until either end has moved enough to change it. The terrain
// comes in through a function so the tests can hand it a made-up landscape.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "radio.h"

namespace xr {
namespace shadow {

// One end of a path: where its aerial is.
struct Endpoint {
    double lat = 0.0, lon = 0.0;
    double antMslM = 0.0;
};

// The terrain at (lat, lon): metres MSL, and whether it is water. False where
// no scenery is loaded -- beyond what X-Plane keeps around the aircraft -- and
// such a sample simply does not count.
using TerrainFn = std::function<bool(double lat, double lon, double* groundMslM, bool* wet)>;

constexpr double kStepKm     = 1.0;     // a probe every kilometre...
constexpr int    kMaxSamples = 240;     // ...and never more than this on one path
constexpr double kRefreshS   = 2.5;     // look again this often, if either end has moved
constexpr double kMovedM     = 300.0;   // ...this far across
constexpr double kClimbedM   = 30.0;    // ...or this far up or down

class Tracker {
public:
    explicit Tracker(TerrainFn terrain) : terrain_(std::move(terrain)) {}

    // This path matters now -- call it every frame for each station that may
    // be heard. `urgent`: it is talking, or tuned; it goes before the rest.
    void want(uint32_t id, const Endpoint& me, const Endpoint& them, uint32_t freqKhz, bool urgent, double now);
    // Spends at most `maxProbes` probes, and stops early past `maxMs` of work,
    // on the path that needs it most. Returns how many probes it made.
    int tick(double now, int maxProbes, double maxMs);
    // The last finished answer for this path; *known false until there is one.
    radio::Shadow result(uint32_t id, bool* known = nullptr) const;
    // Paths nobody has asked about for `idleS` go.
    void forgetIdle(double now, double idleS);
    void clear() { paths_.clear(); haveBusy_ = false; }

    size_t    paths() const { return paths_.size(); }
    long long probes() const { return probes_; }

private:
    struct Path {
        Endpoint me, them;                   // as last asked for
        uint32_t freq = 0;
        bool     urgent = false;
        double   askedAt = 0.0;
        // the run in progress
        bool     running = false;
        Endpoint runMe, runThem;
        uint32_t runFreq = 0;
        double   runKm = 0.0;
        int      next = 0;
        std::vector<radio::ProfilePoint> pts;
        // the last answer, and what it was for
        bool     have = false;
        double   doneAt = -1e9;
        Endpoint doneMe, doneThem;
        uint32_t doneFreq = 0;
        std::vector<radio::ProfilePoint> donePts;
        double   doneKm = 0.0;
        radio::Shadow result;
    };
    bool needsRun(const Path& p, double now) const;
    void start(Path& p);
    void finish(Path& p, double now);

    TerrainFn terrain_;
    std::map<uint32_t, Path> paths_;
    uint32_t  busy_ = 0;                     // the path being sampled...
    bool      haveBusy_ = false;             // ...if there is one
    long long probes_ = 0;
};

// Where a point a fraction `f` of the way along the great circle from a to b is.
void along(double lat1, double lon1, double lat2, double lon2, double f, double* lat, double* lon);

}  // namespace shadow
}  // namespace xr
