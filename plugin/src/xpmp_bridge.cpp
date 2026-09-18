#define _USE_MATH_DEFINES   // MSVC needs this before <cmath> for M_PI

#include "xpmp_bridge.h"
#include "protocol.h"

#include "XPLMUtilities.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef XRADIO_USE_XPMP2
#  include "XPMPAircraft.h"
#  include "XPMPMultiplayer.h"
#  include <map>
#  include <memory>
#endif

namespace xr {
namespace csl {

namespace {

void logMsg(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[600];
    snprintf(line, sizeof(line), "XRadio/CSL: %s\n", buf);
    XPLMDebugString(line);
}

}  // namespace

#ifndef XRADIO_USE_XPMP2
// ---------------------------------------------------------------------------
// Stub build: no XPMP2 available.
// ---------------------------------------------------------------------------
bool available() { return false; }

bool init(const std::string&, const std::string&, std::string* err) {
    if (err) *err = "built without XPMP2 (configure with -DXRADIO_USE_XPMP2=ON)";
    return false;
}

void enable()    {}
void disable()   {}
void shutdown()  {}
void upsert(const RemoteState&) {}
void remove(uint32_t) {}
void removeAll() {}
int  cslModelCount() { return 0; }

#else
// ---------------------------------------------------------------------------
// Real build against XPMP2.
// ---------------------------------------------------------------------------
namespace {

bool g_ready   = false;   // XPMPMultiplayerInit succeeded
bool g_enabled = false;
int  g_models  = 0;

// One remote pilot, rendered by XPMP2 as a CSL model.
class XRAircraft : public XPMP2::Aircraft {
public:
    XRAircraft(const std::string& icaoType, const std::string& callsign, XPMPPlaneID id)
        : XPMP2::Aircraft(icaoType.empty() ? std::string("ZZZZ") : icaoType,
                          /*icaoAirline=*/"", /*livery=*/"", id) {
        label = callsign;
        strncpy(acInfoTexts.tailNum,  callsign.c_str(), sizeof(acInfoTexts.tailNum)  - 1);
        strncpy(acInfoTexts.icaoAcType, icaoType.c_str(), sizeof(acInfoTexts.icaoAcType) - 1);
    }

    // Last state received from the server.
    RemoteState st;

    // Dead reckoning: positions arrive at 5 Hz but this is called every drawn
    // frame, so without extrapolation other aircraft visibly step forward 5
    // times a second. We advance along the last known velocity vector and
    // stop extrapolating if the updates dry up, so a dropped connection
    // freezes the aircraft instead of flying it off across the scenery.
    float sinceNet   = 0.f;   // seconds since the last network update
    float vsFtPerSec = 0.f;   // vertical speed, derived from successive alts
    bool  haveVs     = false;

    static constexpr float kMaxExtrapolationS = 2.0f;

    void ApplyNetworkUpdate(const RemoteState& s) {
        if (sinceNet > 0.01f && sinceNet < kMaxExtrapolationS) {
            vsFtPerSec = (s.altFt - st.altFt) / sinceNet;
            haveVs = true;
        }
        st = s;
        sinceNet = 0.f;
    }

    // XPMP2 calls this once per drawn frame.
    void UpdatePosition(float elapsedSinceLastCall, int) override {
        sinceNet += elapsedSinceLastCall;
        const float dt = std::min(sinceNet, kMaxExtrapolationS);

        double lat = st.lat, lon = st.lon;
        float  alt = st.altFt;

        if (st.gsKt > 1.f) {
            const double distM  = (double)st.gsKt * 0.514444 * dt;
            const double hdgRad = (double)st.heading * M_PI / 180.0;
            const double cosLat = cos(st.lat * M_PI / 180.0);
            lat += (distM * cos(hdgRad)) / 111320.0;
            if (fabs(cosLat) > 1e-6) {
                lon += (distM * sin(hdgRad)) / (111320.0 * cosLat);
            }
            if (haveVs && !st.onGround) alt += vsFtPerSec * dt;
        }

        SetLocation(lat, lon, alt, st.onGround);
        drawInfo.pitch   = st.pitch;
        drawInfo.roll    = st.roll;
        drawInfo.heading = st.heading;

        SetGearRatio(st.gear);
        SetFlapRatio(st.flap);
        // Slats and spoilers are not on the wire yet; follow the flaps so the
        // model does not look frozen.
        SetSlatRatio(st.flap);

        SetLightsNav     (st.lights & LT_NAV);
        SetLightsBeacon  (st.lights & LT_BEACON);
        SetLightsStrobe  (st.lights & LT_STROBE);
        SetLightsLanding (st.lights & LT_LANDING);
        SetLightsTaxi    (st.lights & LT_TAXI);

        // Spin the props/fans so it does not look parked in mid-air.
        SetEngineRotRpm(st.gsKt > 5.f ? 1200.f : 0.f);
        SetPropRotRpm  (st.gsKt > 5.f ? 1200.f : 0.f);
        SetThrustRatio (st.onGround ? 0.2f : 0.8f);

        // Highlight whoever is transmitting on a frequency we monitor.
        if (st.txActive) {
            label = st.callsign + "  [TX]";
            colLabel[0] = 0.3f; colLabel[1] = 1.0f; colLabel[2] = 0.3f;
        } else {
            label = st.callsign;
            colLabel[0] = colLabel[1] = colLabel[2] = 1.0f;
        }
    }
};

std::map<uint32_t, std::unique_ptr<XRAircraft>> g_planes;

// XPMP2 asks us for its settings through this.
int prefsCb(const char* section, const char* key, int dflt) {
    if (!strcmp(section, "planes")) {
        if (!strcmp(key, "clamp_all_to_ground")) return 1;  // keep models out of the dirt
        if (!strcmp(key, "handle_dup_id"))       return 1;  // survive mode-S collisions
    }
    if (!strcmp(section, "debug") && !strcmp(key, "log_level")) {
        return 2;   // warnings and errors only
    }
    return dflt;
}

// Mode-S ids must be 24 bit and non-zero. Session ids are small, so map them
// into a private block that will not collide with real-world traffic.
XPMPPlaneID modeSFor(uint32_t sid) {
    return (XPMPPlaneID)(0xF00000u | (sid & 0x0FFFFFu));
}

}  // namespace

// Only true once aircraft can actually be drawn -- init alone is not enough,
// XPMPMultiplayerEnable has to have taken the AI planes as well.
bool available() { return g_ready && g_enabled; }

bool init(const std::string& pluginRoot, const std::string& defaultIcao, std::string* err) {
    if (g_ready) return true;

    const std::string resourceDir = pluginRoot + "/Resources";

    const char* res = XPMPMultiplayerInit("XRadio", resourceDir.c_str(), prefsCb,
                                          defaultIcao.c_str(), "XR");
    if (res && *res) {
        if (err) *err = std::string("XPMPMultiplayerInit: ") + res;
        logMsg("init failed: %s", res);
        return false;
    }

    // CSL models live in Resources/CSL. Each subfolder with an xsb_aircraft.txt
    // is a package; XPMPLoadCSLPackage walks the tree.
    const std::string cslDir = resourceDir + "/CSL";
    res = XPMPLoadCSLPackage(cslDir.c_str());
    if (res && *res) {
        logMsg("no CSL models in %s (%s) -- traffic will use the default model",
               cslDir.c_str(), res);
    }
    g_models = (int)XPMPGetNumberOfInstalledModels();
    logMsg("initialised, %d CSL models loaded", g_models);

    g_ready = true;
    return true;
}

void enable() {
    if (!g_ready || g_enabled) return;
    const char* res = XPMPMultiplayerEnable();
    if (res && *res) {
        logMsg("enable failed: %s", res);
        return;
    }
    g_enabled = true;
    logMsg("drawing enabled, AI planes acquired");
}

void disable() {
    if (!g_ready) return;
    removeAll();
    if (g_enabled) {
        XPMPMultiplayerDisable();
        g_enabled = false;
    }
}

void shutdown() {
    if (!g_ready) return;
    disable();
    XPMPMultiplayerCleanup();
    g_ready = false;
}

void upsert(const RemoteState& s) {
    if (!g_ready || !g_enabled) return;

    auto it = g_planes.find(s.sid);
    if (it == g_planes.end()) {
        try {
            auto ac = std::make_unique<XRAircraft>(s.acIcao, s.callsign, modeSFor(s.sid));
            ac->st = s;             // seed directly: no previous sample to derive from
            ac->sinceNet = 0.f;
            g_planes[s.sid] = std::move(ac);
            logMsg("added %s (%s) sid=%u", s.callsign.c_str(), s.acIcao.c_str(),
                   (unsigned)s.sid);
        } catch (const XPMP2::XPMP2Error& e) {
            logMsg("cannot create aircraft for %s: %s", s.callsign.c_str(), e.what());
        }
        return;
    }
    it->second->ApplyNetworkUpdate(s);
}

void remove(uint32_t sid) {
    auto it = g_planes.find(sid);
    if (it == g_planes.end()) return;
    logMsg("removed %s sid=%u", it->second->st.callsign.c_str(), (unsigned)sid);
    g_planes.erase(it);   // the Aircraft destructor tears down the instance
}

void removeAll() { g_planes.clear(); }

int cslModelCount() { return g_models; }

#endif  // XRADIO_USE_XPMP2

}  // namespace csl
}  // namespace xr
