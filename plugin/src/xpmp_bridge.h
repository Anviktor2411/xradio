// Thin wrapper around XPMP2 so main.cpp does not have to know the library.
//
// When XRADIO_USE_XPMP2 is off (no lib/XPMP2 checked out yet) every function
// here compiles to a no-op, so the plugin still builds and shows the text
// traffic list.
#pragma once

#include <cstdint>
#include <string>

namespace xr {

struct RemoteState {
    uint32_t    sid      = 0;
    std::string callsign;
    std::string acIcao;
    std::string livery;           // livery folder name, for CSL matching; may be empty
    double      lat      = 0.0;
    double      lon      = 0.0;
    float       altFt    = 0.f;
    float       heading  = 0.f;   // degrees true
    float       pitch    = 0.f;
    float       roll     = 0.f;
    float       gsKt     = 0.f;
    float       gear     = 0.f;   // 0..1
    float       flap     = 0.f;   // 0..1
    uint8_t     lights   = 0;     // xr::LT_* bits
    bool        onGround = false;
    bool        txActive = false;
    uint32_t    timeMs   = 0;     // sender's clock, for interpolation
    float       track    = 0.f;   // degrees true, direction of travel
    float       vsFps    = 0.f;   // vertical speed, ft/s
};

namespace csl {

// Returns true if XPMP2 was compiled in AND initialised successfully.
bool available();

// Called once from XPluginStart. `pluginRoot` is the folder holding the
// platform subdirectories, i.e. .../Resources/plugins/XRadio.
// On failure the reason lands in `err` and the plugin keeps running without
// 3D traffic.
// `extraCslDir` is the folder the pilot named in Settings, empty for none.
// When neither our own Resources/CSL nor that folder yields a model, init
// looks in the CSL libraries other traffic plugins install (xPilot,
// LiveTraffic, IVAO and friends) rather than leaving the sky empty.
bool init(const std::string& pluginRoot, const std::string& defaultIcao,
          const std::string& extraCslDir, std::string* err);

void enable();    // XPluginEnable: start drawing, take over AI planes
void disable();   // XPluginDisable: remove all aircraft, release AI planes
void shutdown();  // XPluginStop

// Create-or-update the aircraft for this session id.
void upsert(const RemoteState& s);

// Remove one aircraft, or all of them.
void remove(uint32_t sid);
void removeAll();

// How many CSL models were loaded, for the status window, and the name of
// the library they came from ("xPilot", "XRadio", ...) or "" for none.
int cslModelCount();
std::string cslModelSource();

// Settings: draw other aircraft at all, and how their labels behave.
void setTrafficVisible(bool on);
void setLabels(bool on, float maxDistNm);

}  // namespace csl
}  // namespace xr
