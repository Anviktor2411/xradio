// Where the ground actually is, asked of X-Plane rather than worked out.
//
// Placing another pilot's aeroplane on the tarmac used to be arithmetic on
// datarefs: take the datum altitude, subtract y_agl, and trust that both mean
// what the documentation says they mean. They do not always, and the failure
// is not symmetric. XPMP2's ground clamp only ever *lifts* a model that has
// come out too low; one that comes out too high is left where it is. So a
// metre of error downwards is invisible and a metre upwards is an aeroplane
// that hovers over the apron for the whole flight.
//
// Asking the sim where the ground is removes both the arithmetic and the
// trust. It also fixes something the arithmetic could never fix: two pilots
// at the same airport rarely have the same scenery installed, so the sender's
// idea of the elevation there can be several metres from the receiver's. The
// aeroplane belongs on the tarmac you can see, not on theirs.
#pragma once

#include <cmath>
#include <cstring>

#include "XPLMGraphics.h"
#include "XPLMScenery.h"

namespace xr {

// Metres MSL of the terrain under (lat, lon). `nearAltM` only has to be in
// the right neighbourhood -- it is where the probe starts looking.
// Returns false if there is no probe or it missed, leaving *out untouched.
// Main thread only: it calls into the XPLM SDK.
inline bool groundElevM(double lat, double lon, double nearAltM, double* out) {
    static XPLMProbeRef probe = nullptr;
    if (!probe) probe = XPLMCreateProbe(xplm_ProbeY);
    if (!probe) return false;

    double x = 0, y = 0, z = 0;
    XPLMWorldToLocal(lat, lon, nearAltM, &x, &y, &z);

    XPLMProbeInfo_t info;
    memset(&info, 0, sizeof(info));
    info.structSize = sizeof(info);
    if (XPLMProbeTerrainXYZ(probe, (float)x, (float)y, (float)z, &info) != xplm_ProbeHitTerrain)
        return false;

    double glat = 0, glon = 0, galt = 0;
    XPLMLocalToWorld(info.locationX, info.locationY, info.locationZ, &glat, &glon, &galt);
    if (!std::isfinite(galt)) return false;
    *out = galt;
    return true;
}

// The most an aeroplane's datum point can plausibly sit above the ground it
// is standing on. An A380's is about eight metres; thirty is generous and
// still catches a probe that hit something on the other side of the world.
inline constexpr double kMaxDatumAboveGroundM = 30.0;

}  // namespace xr
