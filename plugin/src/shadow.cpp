#include "shadow.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "mathconst.h"

namespace xr {
namespace shadow {

namespace {

double metresApart(const Endpoint& a, const Endpoint& b) {
    return radio::distanceNm(a.lat, a.lon, b.lat, b.lon) * 1852.0;
}

// Far enough from where the last answer was worked out that it may be wrong.
bool moved(const Endpoint& now, const Endpoint& then) {
    return metresApart(now, then) > kMovedM || std::fabs(now.antMslM - then.antMslM) > kClimbedM;
}

}  // namespace

void along(double lat1, double lon1, double lat2, double lon2, double f, double* lat, double* lon) {
    const double p1 = lat1 * kPi / 180.0, l1 = lon1 * kPi / 180.0;
    const double p2 = lat2 * kPi / 180.0, l2 = lon2 * kPi / 180.0;
    const double sp = std::sin((p2 - p1) / 2), sl = std::sin((l2 - l1) / 2);
    const double h = sp * sp + std::cos(p1) * std::cos(p2) * sl * sl;
    const double d = 2.0 * std::asin(std::min(1.0, std::sqrt(h)));   // the angle between them
    if (d < 1e-9) {
        *lat = lat1 + (lat2 - lat1) * f;
        *lon = lon1 + (lon2 - lon1) * f;
        return;
    }
    const double a = std::sin((1.0 - f) * d) / std::sin(d), b = std::sin(f * d) / std::sin(d);
    const double x = a * std::cos(p1) * std::cos(l1) + b * std::cos(p2) * std::cos(l2);
    const double y = a * std::cos(p1) * std::sin(l1) + b * std::cos(p2) * std::sin(l2);
    const double z = a * std::sin(p1) + b * std::sin(p2);
    *lat = std::atan2(z, std::sqrt(x * x + y * y)) * 180.0 / kPi;
    *lon = std::atan2(y, x) * 180.0 / kPi;
}

void Tracker::want(uint32_t id, const Endpoint& me, const Endpoint& them, uint32_t freqKhz, bool urgent,
                   double now) {
    Path& p = paths_[id];
    p.me = me; p.them = them; p.freq = freqKhz; p.urgent = urgent; p.askedAt = now;
    // Only the frequency changed: the terrain is the same, so the answer is
    // worked out again from the last profile at once, without a probe.
    if (p.have && !p.running && freqKhz != p.doneFreq && !moved(me, p.doneMe) && !moved(them, p.doneThem)) {
        p.result = radio::shadowOf(p.donePts.data(), (int)p.donePts.size(), p.doneKm,
                                   p.doneMe.antMslM, p.doneThem.antMslM, freqKhz, false);
        p.doneFreq = freqKhz;
    }
}

bool Tracker::needsRun(const Path& p, double now) const {
    if (p.running) return false;
    if (!p.have) return true;
    if (now - p.doneAt < kRefreshS) return false;
    return moved(p.me, p.doneMe) || moved(p.them, p.doneThem);
}

void Tracker::start(Path& p) {
    p.running = true;
    p.runMe = p.me; p.runThem = p.them; p.runFreq = p.freq;
    p.runKm = radio::distanceNm(p.me.lat, p.me.lon, p.them.lat, p.them.lon) * radio::kNmKm;
    const int n = std::max(0, std::min(kMaxSamples, (int)std::ceil(p.runKm / kStepKm) - 1));
    p.pts.assign((size_t)n, radio::ProfilePoint{});
    p.next = 0;
}

void Tracker::finish(Path& p, double now) {
    p.running = false;
    p.result = radio::shadowOf(p.pts.data(), (int)p.pts.size(), p.runKm,
                               p.runMe.antMslM, p.runThem.antMslM, p.runFreq, false);
    p.have = true;
    p.doneAt = now;
    p.doneMe = p.runMe; p.doneThem = p.runThem; p.doneFreq = p.runFreq;
    p.donePts.swap(p.pts);
    p.doneKm = p.runKm;
    p.pts.clear();
}

int Tracker::tick(double now, int maxProbes, double maxMs) {
    const auto t0 = std::chrono::steady_clock::now();
    auto late = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > maxMs;
    };
    int done = 0;
    while (done < maxProbes && !late()) {
        Path* p = nullptr;
        if (haveBusy_) {
            auto it = paths_.find(busy_);
            if (it != paths_.end() && it->second.running) p = &it->second;
            else haveBusy_ = false;
        }
        if (!p) {
            // the next one: the talking and tuned first, then whichever answer is oldest
            const Path* best = nullptr;
            uint32_t pick = 0;
            for (const auto& kv : paths_) {
                const Path& c = kv.second;
                if (!needsRun(c, now)) continue;
                if (!best || (c.urgent && !best->urgent) || (c.urgent == best->urgent && c.doneAt < best->doneAt)) {
                    best = &c;
                    pick = kv.first;
                }
            }
            if (!best) break;
            busy_ = pick;
            haveBusy_ = true;
            p = &paths_[pick];
            start(*p);
        }
        while (done < maxProbes && p->next < (int)p->pts.size()) {
            const int k = p->next;
            const double f = (double)(k + 1) / (double)(p->pts.size() + 1);
            double lat = 0.0, lon = 0.0, ground = 0.0;
            bool wet = false;
            along(p->runMe.lat, p->runMe.lon, p->runThem.lat, p->runThem.lon, f, &lat, &lon);
            radio::ProfilePoint& pt = p->pts[(size_t)k];
            pt.km = f * p->runKm;
            pt.known = terrain_ && terrain_(lat, lon, &ground, &wet);
            pt.groundM = ground;
            pt.wet = wet;
            ++p->next;
            ++done;
            ++probes_;
            if ((done & 7) == 0 && late()) break;
        }
        if (p->next < (int)p->pts.size()) break;      // out of probes or time: carry on next frame
        finish(*p, now);
        haveBusy_ = false;
    }
    return done;
}

radio::Shadow Tracker::result(uint32_t id, bool* known) const {
    auto it = paths_.find(id);
    const bool have = it != paths_.end() && it->second.have;
    if (known) *known = have;
    return have ? it->second.result : radio::Shadow{};
}

void Tracker::forgetIdle(double now, double idleS) {
    for (auto it = paths_.begin(); it != paths_.end();) {
        if (now - it->second.askedAt > idleS) {
            if (haveBusy_ && busy_ == it->first) haveBusy_ = false;
            it = paths_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace shadow
}  // namespace xr
