// The VHF radio model: how far a transmission reaches, and how it sounds when
// it gets there. docs/radio.md has the whole story with worked examples.
//
// One header with no X-Plane in it, because three things have to agree on it
// exactly: the server built into the plugin, which relays a transmission only
// to who can receive it; the plugin, which turns the same numbers into noise,
// flutter and break-up; and the tests. server/server.py carries the same model
// line for line, and tools/check_sizes.py compares the two on a table of
// cases -- the servers deciding differently would mean a station heard in one
// flight and silent in another.
//
// In short:
//   * the horizon is 1.23 (sqrt h1 + sqrt h2) nm, h in feet ABOVE THE GROUND
//     plus the aerial: 3 m on an aircraft, the station's mast or remote site
//     for ATC. Never less than 5 nm, so one airport always works;
//   * an ATC station also covers its service volume (a tower 25 nm and 4000 ft
//     above it, approach 60 nm, a centre its whole area), horizon or not;
//   * inside that, the signal is a link budget -- transmitter power less free
//     space path loss against the receiver's noise -- faded towards the edge of
//     the coverage, where it ends;
//   * the plugin then takes off what the terrain on the path costs (it has the
//     scenery; the server has not), and the voice code makes two transmissions
//     at once fight it out by level (the capture effect).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mathconst.h"
#include "protocol.h"   // AtcKind, WeatherPayload

namespace xr {
namespace radio {

// ---------------------------------------------------------------------------
// heights and the horizon
// ---------------------------------------------------------------------------
constexpr double kFtPerM           = 3.28084;
constexpr double kNmKm             = 1.852;
constexpr double kAircraftAntennaM = 3.0;    // a blade aerial on the fuselage, ~10 ft
// The radio horizon over an earth bent by refraction to 4/3 of its radius,
// in nautical miles per square root of feet: d = 1.23 (sqrt h1 + sqrt h2).
constexpr double kHorizonK         = 1.23;
// However low both aerials are, this close always works: an apron, a taxiway
// and the runway of one airport are never further apart. Two aircraft on the
// ground (3 m aerials) get 7.7 nm from the horizon itself; the floor is for
// the corners -- a sunk apron, a probe that put the ground a metre high.
constexpr double kRangeFloorNm     = 5.0;

// What an ATC station is assumed to have when it does not say: the height of
// its antenna above the terrain around it (the horizon counts from there), and
// its service volume -- everyone within rangeNm and below ceilingFt above the
// station is covered whatever the horizon says, the way real facilities place
// their antennas (approach and centre on remote sites, several of them) so the
// whole of their airspace hears them. A station can send its own (PositionPayload
// aglM, svcRangeNm, svcCeilingFt).
struct StationDefaults { double antennaM; double rangeNm; double ceilingFt; };
inline StationDefaults stationDefaults(uint8_t kind) {
    switch (kind) {
        case ATC_DEL:  return {30.0, 10.0, 2000.0};
        case ATC_GND:  return {30.0, 10.0, 2000.0};
        case ATC_TWR:  return {45.0, 25.0, 4000.0};
        case ATC_APP:  return {300.0, 60.0, 25000.0};
        case ATC_CTR:  return {600.0, 250.0, 65000.0};   // a centre's whole area (its FIR)
        case ATC_ATIS: return {45.0, 60.0, 20000.0};
        default:       return {30.0, 0.0, 0.0};          // unknown: its antenna, no service volume
    }
}

// One end of a link, as the model needs it.
struct Node {
    double lat = 0.0, lon = 0.0;   // degrees
    double altMslM = 0.0;          // where it is
    double heightFt = 0.0;         // its aerial above the terrain around it
    bool   atc = false;            // a ground station: 25 W instead of 10, a service volume
    double rangeNm = 0.0;          // service volume (stations), 0 = none
    double ceilingFt = 0.0;        // ...up to this many feet above the station
};

// An aircraft's aerial, from its height above the ground.
inline double aircraftHeightFt(double aglM) {
    return (std::max(0.0, aglM) + kAircraftAntennaM) * kFtPerM;
}

// The height above the ground the radio counts for an aircraft. A v6 client
// reports it; for one that does not (a v5 client, a 108-byte injected entry)
// it is 0 on the ground and -- in the air, where nothing better is known --
// the altitude, which is what every version before v6 used.
inline double effectiveAglM(bool known, double aglM, bool onGround, double altMslM) {
    if (known) return aglM;
    return onGround ? 0.0 : altMslM;
}

// The great-circle distance in nm (haversine), the one both servers have
// always used.
inline double distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double rNm = 3440.065;
    const double p1 = lat1 * kPi / 180.0, p2 = lat2 * kPi / 180.0;
    const double dp = p2 - p1;
    const double dl = (lon2 - lon1) * kPi / 180.0;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * rNm * std::asin(std::min(1.0, std::sqrt(h)));
}

inline double horizonNm(double h1Ft, double h2Ft) {
    const double los = kHorizonK * (std::sqrt(std::max(0.0, h1Ft)) + std::sqrt(std::max(0.0, h2Ft)));
    return std::max(kRangeFloorNm, los);
}

// Tropospheric ducting (docs/radio.md): on a warm, still evening a layer of
// air can trap VHF and carry it far past the horizon over the sea. When the
// flight has it, links between two aerials low enough to be inside the duct
// reach this much further. The server cannot see the sea, so it relays the
// whole extension; the plugin, which can, only hears it over water.
constexpr double kDuctFactor       = 2.0;
constexpr double kDuctMaxHeightFt  = 6000.0;

// The horizon between these two, longer in a duct.
inline double horizonFor(const Node& a, const Node& b, bool ducting) {
    double r = horizonNm(a.heightFt, b.heightFt);
    if (ducting && a.heightFt <= kDuctMaxHeightFt && b.heightFt <= kDuctMaxHeightFt) r *= kDuctFactor;
    return r;
}

// A station's service volume radius when the other end is an aircraft below
// its ceiling; 0 otherwise.
inline double serviceRangeFor(const Node& a, const Node& b) {
    if (a.atc == b.atc) return 0.0;
    const Node& st = a.atc ? a : b;
    const Node& ac = a.atc ? b : a;
    if (st.rangeNm > 0.0 && ac.altMslM * kFtPerM <= st.altMslM * kFtPerM + st.ceilingFt) return st.rangeNm;
    return 0.0;
}

// How far a link between these two reaches: the horizon, or a station's
// service volume for an aircraft inside it, whichever is further.
inline double coverageNm(const Node& a, const Node& b, bool ducting) {
    return std::max(horizonFor(a, b, ducting), serviceRangeFor(a, b));
}

// Inside its station's service volume: what the facility's own antennas are
// placed to cover, so the plugin does not shadow it behind terrain either.
inline bool inServiceVolume(const Node& a, const Node& b) {
    const double rs = serviceRangeFor(a, b);
    return rs > 0.0 && distanceNm(a.lat, a.lon, b.lat, b.lon) <= rs;
}

// ---------------------------------------------------------------------------
// the link budget
// ---------------------------------------------------------------------------
constexpr double kTxAircraftDbm = 40.0;      // 10 W, a panel-mounted COM
constexpr double kTxStationDbm  = 43.9794;   // 25 W, a ground station (10 log10 25000 mW)
// The receiver's noise in a 25 kHz channel with a 7 dB noise figure. A signal
// 6 dB over it (-107 dBm, about 2 microvolts) is where a COM's sensitivity is
// specified; 3 dB over it is nothing worth hearing, 33 dB over it is clean.
constexpr double kNoiseDbm      = -113.0;
constexpr double kSnrZeroDb     = 3.0;
constexpr double kSnrCleanDb    = 33.0;
// Clean up to this share of the horizon, then fading to nothing at it: the
// path grazing the earth's bulge, then going under it.
constexpr double kFadeFrom      = 0.6;
// A service volume is what a facility's antennas are placed to cover, so it is
// full strength across it and fades only in its last tenth.
constexpr double kServiceFadeFrom = 0.9;
// Two transmissions at once: the stronger by more than this captures the
// receiver and the weaker is only a whine underneath it; closer than this they
// garble each other (voice.cpp).
constexpr double kCaptureDb     = 6.0;

inline double smoothstep01(double t) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    return t * t * (3.0 - 2.0 * t);
}

// The transmission's frequency for the path loss, kept inside the airband so a
// client sending nonsense cannot produce a log of zero.
inline double airbandMhz(uint32_t freqKhz) {
    const double f = (double)freqKhz / 1000.0;
    return std::min(160.0, std::max(100.0, f));
}

// Free space path loss, dB: 32.45 + 20 log10(f MHz) + 20 log10(d km).
inline double fsplDb(double mhz, double km) {
    return 32.45 + 20.0 * std::log10(mhz) + 20.0 * std::log10(std::max(km, 0.05));
}

struct Link {
    double distNm = 0.0;
    double coverageNm = 0.0;
    double levelDbm = -200.0;  // what arrives, faded towards the edge: for comparing two signals
    double snrDb = -200.0;     // ...over the receiver's noise, before the fade
    double fade = 0.0;         // 1 well inside the coverage, 0 at its edge
    double quality = 0.0;      // 0..1: 1 clean, 0 nothing. > 0 means the server relays it
};

// From `tx` to `rx` on `freqKhz`. `terrainLossDb` is the plugin's: the server
// passes 0, which is the one thing that may let a station through that the
// receiving plugin then loses behind a mountain.
inline Link link(const Node& tx, const Node& rx, uint32_t freqKhz, bool ducting, double terrainLossDb = 0.0) {
    Link l;
    l.distNm = distanceNm(tx.lat, tx.lon, rx.lat, rx.lon);
    const double rh = horizonFor(tx, rx, ducting), rs = serviceRangeFor(tx, rx);
    l.coverageNm = std::max(rh, rs);
    if (l.distNm >= l.coverageNm) return l;                    // past it: nothing at all
    const double fadeH = 1.0 - smoothstep01((l.distNm / rh - kFadeFrom) / (1.0 - kFadeFrom));
    const double fadeS = rs > 0.0
        ? 1.0 - smoothstep01((l.distNm / rs - kServiceFadeFrom) / (1.0 - kServiceFadeFrom)) : 0.0;
    l.fade = std::max(fadeH, fadeS);
    const double pRx = (tx.atc ? kTxStationDbm : kTxAircraftDbm) -
                       fsplDb(airbandMhz(freqKhz), l.distNm * kNmKm) - terrainLossDb;
    l.snrDb = pRx - kNoiseDbm;
    l.quality = smoothstep01((l.snrDb - kSnrZeroDb) / (kSnrCleanDb - kSnrZeroDb)) * l.fade;
    l.levelDbm = pRx + 20.0 * std::log10(std::max(l.fade, 1e-3));
    return l;
}

// ---------------------------------------------------------------------------
// tropospheric ducting, from the weather the flight shares
// ---------------------------------------------------------------------------
// Warm, high pressure, little wind and dry: the evening a temperature
// inversion sits over the sea. Rare even then -- a third of the hours of such
// a day, picked from the sim's date and hour so every server, and every
// receiver, agrees which ones.
inline bool ductingWeather(const WeatherPayload& w) {
    // compared as doubles, exactly as Python compares the unpacked floats
    return (double)w.seaLevelTempC >= 20.0 && (double)w.seaLevelPressurePa >= 101500.0 &&
           (double)w.windSpeedMs[0] <= 6.0 && (double)w.rainPercent < 0.1;
}
inline bool ductingHour(int32_t dateDays, float zuluTimeSec) {
    const long long hour = (long long)dateDays * 24 + (long long)std::floor((double)zuluTimeSec / 3600.0);
    const uint32_t h = (uint32_t)((uint64_t)hour * 2654435761ull);
    return h < 0x59999999u;            // ~35 % of the hours
}
inline bool ductingFrom(const WeatherPayload& w) {
    return ductingWeather(w) && ductingHour(w.dateDays, w.zuluTimeSec);
}

// ---------------------------------------------------------------------------
// terrain on the path (the plugin samples the scenery; this is the arithmetic)
// ---------------------------------------------------------------------------
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kRefractionK   = 4.0 / 3.0;   // VHF bends around the earth this much
// Deeper than this behind the terrain (the knife-edge parameter nu, ~10 for a
// ridge a kilometre above the line half way along a 30 km path) and nothing
// gets through at all.
constexpr double kDeepNu        = 10.0;

struct ProfilePoint {
    double km = 0.0;            // from the receiver along the path
    double groundM = 0.0;       // terrain there, metres MSL
    bool   wet = false;         // water
    bool   known = false;       // the probe found scenery there
};

struct Shadow {
    double nu = -99.0;          // the worst obstacle's knife-edge parameter: > 0 blocks the line
    double lossDb = 0.0;        // what diffracting over it costs
    double cap = 1.0;           // the most the quality can be behind it
    double wetFraction = 0.0;   // of the sampled path, over water
    bool   blocked = false;
};

// Knife-edge diffraction loss (ITU-R P.526) for the parameter nu.
inline double knifeEdgeDb(double nu) {
    if (nu <= -0.78) return 0.0;
    const double t = nu - 0.1;
    return 6.9 + 20.0 * std::log10(std::sqrt(t * t + 1.0) + t);
}

// The straight line between the two aerials (MSL), over an earth bent to 4/3
// of its radius, against the terrain sampled under it. The worst obstacle
// decides, as a knife edge: grazing it costs 6 dB and caps the signal at
// weak and noisy; deeper behind it the cap falls to nothing at kDeepNu. In a
// duct the bulge goes away -- the duct is what carries the signal round it.
inline Shadow shadowOf(const ProfilePoint* pts, int n, double totalKm, double rxAntMslM, double txAntMslM,
                       uint32_t freqKhz, bool ducting) {
    Shadow s;
    if (totalKm <= 0.0) return s;
    const double lambdaM = 299.792458 / airbandMhz(freqKhz);
    const double kR = kRefractionK * kEarthRadiusKm * 1000.0;
    const double D = totalKm * 1000.0;
    int known = 0, wet = 0;
    for (int i = 0; i < n; ++i) {
        const ProfilePoint& p = pts[i];
        if (!p.known) continue;
        ++known;
        if (p.wet) ++wet;
        const double d1 = p.km * 1000.0, d2 = D - d1;
        if (d1 <= 0.0 || d2 <= 0.0) continue;
        const double bulge = ducting ? 0.0 : d1 * d2 / (2.0 * kR);
        const double line = rxAntMslM + (txAntMslM - rxAntMslM) * d1 / D;
        const double h = p.groundM + bulge - line;              // above the line: in the way
        const double nu = h * std::sqrt(2.0 * D / (lambdaM * d1 * d2));
        if (nu > s.nu) s.nu = nu;
    }
    s.wetFraction = known ? (double)wet / (double)known : 0.0;
    s.lossDb = knifeEdgeDb(s.nu);
    s.blocked = s.nu > 0.0;
    if (s.blocked) s.cap = std::max(0.0, 0.45 * (1.0 - s.nu / kDeepNu));
    return s;
}

}  // namespace radio
}  // namespace xr
