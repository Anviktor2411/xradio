// Turns a trickle of timestamped position reports into smooth motion.
//
// The problem it solves: reports arrive ~5 times a second, through a relay
// that samples on its own clock, over a network with jitter. Drawn as-is the
// aircraft steps, freezes, and jumps -- and any scheme that restarts a clock
// on packet arrival turns a duplicated report into a jump *backwards*.
//
// The fix is the one every multiplayer game uses: every report carries the
// sender's own timestamp, and we render a fixed delay behind the newest one,
// interpolating between the two reports that bracket the render time. Arrival
// timing then no longer matters at all; only the sender's timeline does.
// If the buffer runs dry we extrapolate along the reported velocity for a
// bounded time, then hold.
#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include "mathconst.h"

namespace xr {

struct PoseSample {
    double t = 0;           // sender time, seconds, unwrapped and monotonic
    double lat = 0, lon = 0;
    float  altFt = 0;
    float  heading = 0, pitch = 0, roll = 0;
    float  track = 0;       // direction of travel, degrees true
    float  gsKt = 0;
    float  vsFps = 0;       // vertical speed, ft/s
};

struct Pose {
    double lat = 0, lon = 0;
    float  altFt = 0, heading = 0, pitch = 0, roll = 0;
};

class Smoother {
public:
    // How far behind the newest report we draw. Larger absorbs more jitter,
    // smaller feels more immediate; 350 ms covers the relay's worst case
    // (two 5 Hz samplers beating against each other) with room to spare.
    static constexpr double kPlayoutS   = 0.35;
    static constexpr double kMaxExtrapS = 1.0;    // then hold position
    static constexpr double kSnapS      = 0.75;   // render clock this far off -> jump, do not slew
    static constexpr int    kMaxSamples = 10;

    bool empty() const { return buf_.empty(); }

    // A report from the network. `rawMs` is the sender's uint32 clock.
    void push(uint32_t rawMs, PoseSample s) {
        // unwrap the 32-bit millisecond clock into a continuous double
        if (!haveRaw_) {
            haveRaw_ = true;
            lastRaw_ = rawMs;
        }
        const int32_t delta = (int32_t)(rawMs - lastRaw_);   // wraps correctly
        lastRaw_ = rawMs;
        unwrapped_ += delta / 1000.0;
        s.t = unwrapped_;

        // The relay re-sends the newest report it has at its own rate, so
        // duplicates (same sender time) are normal; drop them, and anything
        // that somehow arrives out of order.
        if (!buf_.empty() && s.t <= buf_.back().t) {
            unwrapped_ = buf_.back().t;         // do not let a dud move the clock
            return;
        }
        buf_.push_back(s);
        while ((int)buf_.size() > kMaxSamples) buf_.pop_front();

        if (!haveRender_) {
            haveRender_ = true;
            renderT_ = s.t - kPlayoutS;
        }
    }

    // Called once per drawn frame with the local time since the last call.
    Pose sample(double elapsedS) {
        if (buf_.empty()) return last_;
        if (elapsedS < 0) elapsedS = 0;
        if (elapsedS > 0.5) elapsedS = 0.5;         // a stall is not a time warp

        // Keep the render clock kPlayoutS behind the newest report by running
        // it slightly fast when it has fallen behind and slightly slow when
        // it has crept ahead. It only ever runs *forward*: pulling it back
        // would move the aircraft backwards, which is the very thing this
        // class exists to prevent. If the stream stalls we simply run on into
        // extrapolation and then hold; when it resumes far ahead of us, jump.
        const double target = buf_.back().t - kPlayoutS;
        const double err = target - renderT_;
        double rate = 1.0;
        if (err > 0.02)       rate = 1.05;
        else if (err < -0.02) rate = 0.95;
        renderT_ += elapsedS * rate;
        if (err > kSnapS) renderT_ = target;

        prune();

        const PoseSample& newest = buf_.back();
        if (renderT_ >= newest.t) {
            const double ahead = renderT_ - newest.t;
            last_ = extrapolate(newest, ahead < kMaxExtrapS ? ahead : kMaxExtrapS);
            return last_;
        }
        if (renderT_ <= buf_.front().t) {
            last_ = poseOf(buf_.front());
            return last_;
        }
        for (size_t i = 0; i + 1 < buf_.size(); ++i) {
            const PoseSample& a = buf_[i];
            const PoseSample& b = buf_[i + 1];
            if (renderT_ >= a.t && renderT_ <= b.t) {
                const double span = b.t - a.t;
                const double f = span > 1e-6 ? (renderT_ - a.t) / span : 1.0;
                last_ = lerp(a, b, f);
                return last_;
            }
        }
        last_ = poseOf(newest);
        return last_;
    }

    // For diagnostics: how far behind the newest report we are drawing, and
    // the instant (in the sender's timeline) the last sample() rendered.
    double lagS() const { return buf_.empty() ? 0.0 : buf_.back().t - renderT_; }
    double renderTime() const { return renderT_; }

private:
    std::deque<PoseSample> buf_;
    double   renderT_ = 0;
    bool     haveRender_ = false;
    uint32_t lastRaw_ = 0;
    double   unwrapped_ = 0;
    bool     haveRaw_ = false;
    Pose     last_;

    void prune() {
        // keep everything the render time might still need, plus one before
        while (buf_.size() > 2 && buf_[1].t < renderT_) buf_.pop_front();
    }

    static Pose poseOf(const PoseSample& s) {
        Pose p;
        p.lat = s.lat; p.lon = s.lon; p.altFt = s.altFt;
        p.heading = s.heading; p.pitch = s.pitch; p.roll = s.roll;
        return p;
    }

    static float lerpAngle(float a, float b, double f) {
        float d = std::fmod(b - a + 540.f, 360.f) - 180.f;   // shortest way round
        float r = a + (float)(d * f);
        if (r < 0.f) r += 360.f;
        if (r >= 360.f) r -= 360.f;
        return r;
    }

    static Pose lerp(const PoseSample& a, const PoseSample& b, double f) {
        Pose p;
        p.lat     = a.lat + (b.lat - a.lat) * f;
        p.lon     = a.lon + (b.lon - a.lon) * f;
        p.altFt   = a.altFt + (float)((b.altFt - a.altFt) * f);
        p.heading = lerpAngle(a.heading, b.heading, f);
        p.pitch   = a.pitch + (float)((b.pitch - a.pitch) * f);
        p.roll    = a.roll  + (float)((b.roll  - a.roll)  * f);
        return p;
    }

    static Pose extrapolate(const PoseSample& s, double dt) {
        Pose p = poseOf(s);
        if (s.gsKt > 1.f && dt > 0) {
            const double distM  = (double)s.gsKt * 0.514444 * dt;
            const double trkRad = (double)s.track * kPi / 180.0;
            const double cosLat = std::cos(s.lat * kPi / 180.0);
            p.lat += (distM * std::cos(trkRad)) / 111320.0;
            if (std::fabs(cosLat) > 1e-6) {
                p.lon += (distM * std::sin(trkRad)) / (111320.0 * cosLat);
            }
            p.altFt += (float)(s.vsFps * dt);
        }
        return p;
    }
};

}  // namespace xr
