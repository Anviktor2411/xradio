// The pilot's own TCAS, as X-Plane 12 (12.4+, "TCAS II" in Plane Maker) shows it, turned into what goes on the
// wire (TcasPayload). No X-Plane in here: main.cpp reads the datarefs and hands the values over, the test feeds
// them by hand. The datarefs (developer.x-plane.com, "TCAS resolution advisories and plugin traffic"):
//
//   sim/cockpit2/tcas/indicators/tcas_active_advisory   1 = traffic advisory, 2 = resolution advisory
//   sim/cockpit2/tcas/indicators/tcas_message           aural message 0..16 (1 climb, 2 crossing climb,
//       3 increase climb, 4 climb now, 5 clear of conflict, 6 descend, 7 crossing descend, 8 increase descend,
//       9 descend now, 10 monitor vertical speed, 11 maintain vertical speed, 12 crossing maintain,
//       13 level off, 14 traffic, 15 test pass, 16 test fail)
//   sim/cockpit2/tcas/indicators/tcas_vs_bands[6]       fpm: lower red, green, upper red (each lower, upper)
//   sim/cockpit2/tcas/targets/threat[64]                per target: 2 TA, 3 preventive RA, 4 corrective RA
//   sim/cockpit2/tcas/targets/modeS_id[64]              per target: the 24-bit id (XPMP2 gives ours)
#pragma once

#include <cmath>
#include <cstdint>

namespace xr {
namespace tcas {

static const float kFpmToMs = 0.00508f;

enum Message : int {
    M_NONE = 0, M_CLIMB = 1, M_CROSS_CLIMB = 2, M_INCREASE_CLIMB = 3, M_CLIMB_NOW = 4, M_CLEAR = 5,
    M_DESCEND = 6, M_CROSS_DESCEND = 7, M_INCREASE_DESCEND = 8, M_DESCEND_NOW = 9, M_MONITOR_VS = 10,
    M_MAINTAIN_VS = 11, M_CROSS_MAINTAIN = 12, M_LEVEL_OFF = 13, M_TRAFFIC = 14, M_TEST_PASS = 15, M_TEST_FAIL = 16,
};

struct State {
    uint8_t level = 0;        // xr::TcasLevel: 0 none, 1 TA, 2 RA, 3 clear of conflict
    int8_t  sense = 0;        // +1 climb, -1 descend, 0 level / maintain / monitor
    uint8_t message = 0;
    float   vsTargetMs = 0;   // the edge of the green band the RA asks for
    int     threatIndex = -1; // the target with the highest threat (index into the target arrays)
    bool operator!=(const State& o) const {
        return level != o.level || sense != o.sense || message != o.message || std::fabs(vsTargetMs - o.vsTargetMs) > 0.5f;
    }
};

inline int8_t senseOf(int message) {
    switch (message) {
        case M_CLIMB: case M_CROSS_CLIMB: case M_INCREASE_CLIMB: case M_CLIMB_NOW: return 1;
        case M_DESCEND: case M_CROSS_DESCEND: case M_INCREASE_DESCEND: case M_DESCEND_NOW: return -1;
        default: return 0;
    }
}

/** advisory: tcas_active_advisory; message: tcas_message (the last non-silent one while the RA lasts);
 *  bands: tcas_vs_bands[6] in fpm (may be null); threats/n: tcas/targets/threat[]. prevLevel: last state's level. */
inline State read(int advisory, int message, const float* bands, const int* threats, int n, uint8_t prevLevel = 0, int8_t prevSense = 0) {
    State s;
    s.message = (uint8_t)(message >= 0 && message <= 16 ? message : 0);
    if (advisory >= 2) s.level = 2;
    else if (advisory == 1) s.level = 1;
    if (message == M_CLEAR || (prevLevel == 2 && advisory < 2)) s.level = advisory == 1 ? 1 : 3;   // the RA just ended
    s.sense = senseOf(message);
    if (s.level == 2 && s.sense == 0 && message == M_NONE) s.sense = prevSense;   // between two aural messages
    if (s.level == 2 && bands) {
        // the green band: [2] lower, [3] upper. Climb: at least its lower edge. Descend: at most its upper edge.
        const float lo = bands[2], hi = bands[3];
        const bool shown = std::isfinite(lo) && std::isfinite(hi) && lo != hi && std::fabs(lo) < 1e30f && std::fabs(hi) < 1e30f;
        if (shown) s.vsTargetMs = (s.sense > 0 ? lo : s.sense < 0 ? hi : (lo + hi) / 2) * kFpmToMs;
        else if (s.sense > 0 && std::fabs(lo) < 1e30f) s.vsTargetMs = lo * kFpmToMs;
        else if (s.sense < 0 && std::fabs(hi) < 1e30f) s.vsTargetMs = hi * kFpmToMs;
    }
    if (s.level == 2 && s.vsTargetMs == 0 && s.sense != 0) {
        const bool more = message == M_INCREASE_CLIMB || message == M_INCREASE_DESCEND;
        s.vsTargetMs = s.sense * (more ? 2500.f : 1500.f) * kFpmToMs;
    }
    int best = 1;
    for (int i = 0; threats && i < n; ++i) if (threats[i] > best) { best = threats[i]; s.threatIndex = i; }
    return s;
}

}  // namespace tcas
}  // namespace xr
