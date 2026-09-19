#include "xpmp_bridge.h"
#include "protocol.h"
#include "mathconst.h"
#include "smoothing.h"

#include "XPLMUtilities.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

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

bool init(const std::string&, const std::string&, const std::string&,
          std::string* err) {
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
std::string cslModelSource() { return ""; }
void setTrafficVisible(bool) {}
void setLabels(bool, float) {}

#else
// ---------------------------------------------------------------------------
// Real build against XPMP2.
// ---------------------------------------------------------------------------
namespace {

bool g_ready   = false;   // XPMPMultiplayerInit succeeded
bool g_enabled = false;
bool g_visible = true;    // the "draw other aircraft" setting
int  g_models  = 0;
std::string g_modelSource;   // which library the models came from, for the UI

// .../Resources/plugins, from .../Resources/plugins/XRadio.
std::string pluginsDir(const std::string& pluginRoot) {
    const size_t cut = pluginRoot.find_last_of("/\\");
    return cut == std::string::npos ? pluginRoot : pluginRoot.substr(0, cut);
}

// Windows hands us a path with backslashes and we append with slashes, so
// logs end up looking like "D:\\SteamLibrary/steamapps/...". Harmless to
// XPMP2, confusing to read in Log.txt.
std::string nativePath(std::string p) {
#ifdef _WIN32
    for (char& c : p) if (c == '/') c = '\\';
#endif
    return p;
}

// Hand a folder to XPMP2 and note whether it produced anything. XPMP2 walks
// the tree itself, so one call covers a whole library of packages.
void loadPackages(const std::string& rawDir, const char* who) {
    const std::string dir = nativePath(rawDir);
    const int before = (int)XPMPGetNumberOfInstalledModels();
    const char* res = XPMPLoadCSLPackage(dir.c_str());
    const int after = (int)XPMPGetNumberOfInstalledModels();
    if (res && *res && after == before) {
        logMsg("nothing in %s (%s)", dir.c_str(), res);
        return;
    }
    if (after > before) {
        logMsg("%d models from %s (%s)", after - before, who, dir.c_str());
        if (g_modelSource.empty()) g_modelSource = who;
    }
    g_models = after;
}

// One remote pilot, rendered by XPMP2 as a CSL model.
class XRAircraft : public XPMP2::Aircraft {
public:
    XRAircraft(const std::string& icaoType, const std::string& livery,
               const std::string& callsign, XPMPPlaneID id)
        : XPMP2::Aircraft(icaoType.empty() ? std::string("ZZZZ") : icaoType,
                          /*icaoAirline=*/"", livery, id) {
        label = callsign;
        strncpy(acInfoTexts.tailNum,  callsign.c_str(), sizeof(acInfoTexts.tailNum)  - 1);
        strncpy(acInfoTexts.icaoAcType, icaoType.c_str(), sizeof(acInfoTexts.icaoAcType) - 1);
    }

    // Last state received from the server (configuration, lights, labels).
    RemoteState st;

    // Position and attitude come from the smoother, which interpolates in
    // the sender's own timeline. See smoothing.h for why: drawing reports as
    // they arrive, or dead-reckoning from each one, stutters.
    Smoother smoother;

    void ApplyNetworkUpdate(const RemoteState& s) {
        st = s;
        PoseSample ps;
        ps.lat = s.lat;   ps.lon = s.lon;   ps.altFt = s.altFt;
        ps.heading = s.heading; ps.pitch = s.pitch; ps.roll = s.roll;
        ps.track = s.track; ps.gsKt = s.gsKt; ps.vsFps = s.vsFps;
        smoother.push(s.timeMs, ps);
    }

    // XPMP2 calls this once per drawn frame.
    void UpdatePosition(float elapsedSinceLastCall, int) override {
        const Pose p = smoother.sample(elapsedSinceLastCall);

        SetLocation(p.lat, p.lon, p.altFt, st.onGround);
        drawInfo.pitch   = p.pitch;
        drawInfo.roll    = p.roll;
        drawInfo.heading = p.heading;

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

bool init(const std::string& pluginRoot, const std::string& defaultIcao,
          const std::string& extraCslDir, std::string* err) {
    if (g_ready) return true;

    const std::string resourceDir = pluginRoot + "/Resources";

    const char* res = XPMPMultiplayerInit("XRadio", resourceDir.c_str(), prefsCb,
                                          defaultIcao.c_str(), "XR");
    if (res && *res) {
        if (err) *err = std::string("XPMPMultiplayerInit: ") + res;
        logMsg("init failed: %s", res);
        return false;
    }

    // Our own Resources/CSL first. Each subfolder with an xsb_aircraft.txt is
    // a package; XPMPLoadCSLPackage walks the tree.
    loadPackages(resourceDir + "/CSL", "XRadio");

    // A folder the pilot typed in wins over anything we go looking for.
    if (!extraCslDir.empty() && g_models == 0)
        loadPackages(extraCslDir, "the folder in Settings");

    // Still nothing: almost every X-Plane install that wants CSL traffic
    // already has a library sitting in another plugin, and asking people to
    // copy a gigabyte of models around to use a second plugin is silly.
    if (g_models == 0) {
        const std::string plugins = pluginsDir(pluginRoot);
        struct Candidate { const char* path; const char* who; };
        static const Candidate kKnown[] = {
            {"/xPilot/Resources/CSL",       "xPilot"},
            {"/LiveTraffic/Resources/CSL",  "LiveTraffic"},
            {"/swift/Resources/CSL",        "swift"},
            {"/XSquawkBox/Resources/CSL",   "XSquawkBox"},
            {"/IVAO_CSL/CSL",               "IVAO Altitude"},
            {"/CSL",                        "Resources/plugins/CSL"},
        };
        for (const auto& c : kKnown) {
            if (g_models > 0) break;
            loadPackages(plugins + c.path, c.who);
        }
    }

    if (g_models == 0)
        logMsg("no CSL models found -- other aircraft will be invisible; "
               "put a package in %s/CSL or name a folder in Settings > Traffic",
               resourceDir.c_str());
    else
        logMsg("initialised, %d CSL models loaded from %s",
               g_models, g_modelSource.c_str());

    g_ready = true;
    return true;
}

void enable() {
    if (!g_ready || g_enabled) return;

    // Taking the AI planes takes TCAS away from every other traffic plugin --
    // only one of us can have it. With no models loaded, or with traffic
    // switched off, we would hold it and draw nothing, so we leave it for
    // whoever can use it (PlanePals, SayIntentions, LiveTraffic, ...).
    if (g_models == 0) {
        logMsg("no CSL models, leaving TCAS and the AI planes to other plugins");
        return;
    }
    if (!g_visible) {
        logMsg("traffic switched off, leaving TCAS to other plugins");
        return;
    }

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

void setTrafficVisible(bool on) {
    if (g_visible == on) return;
    g_visible = on;
    if (!on) {
        removeAll();           // upsert() will recreate them when switched back
        if (g_enabled) {       // and give TCAS back while we are not using it
            XPMPMultiplayerDisable();
            g_enabled = false;
        }
    } else {
        enable();              // takes the AI planes again if we have models
    }
    logMsg("traffic rendering %s", on ? "on" : "off");
}

void setLabels(bool on, float maxDistNm) {
    if (!g_ready) return;
    XPMPEnableAircraftLabels(on);
    if (on) XPMPSetAircraftLabelDist(maxDistNm, true);
}

void upsert(const RemoteState& s) {
    if (!g_ready || !g_enabled || !g_visible) return;

    auto it = g_planes.find(s.sid);
    if (it == g_planes.end()) {
        try {
            auto ac = std::make_unique<XRAircraft>(s.acIcao, s.livery, s.callsign, modeSFor(s.sid));
            ac->ApplyNetworkUpdate(s);
            g_planes[s.sid] = std::move(ac);
            logMsg("added %s (%s %s) sid=%u", s.callsign.c_str(), s.acIcao.c_str(),
                   s.livery.c_str(), (unsigned)s.sid);
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
std::string cslModelSource() { return g_modelSource; }

#endif  // XRADIO_USE_XPMP2

}  // namespace csl
}  // namespace xr
