// Just enough of XPLMScenery for the terrain probe the plugin uses to find
// the ground. The stub in xplm_stub.cpp answers with whatever elevation the
// test set, and can be told to miss.
#ifndef _XPLMScenery_h_
#define _XPLMScenery_h_

#include "XPLMDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* XPLMProbeRef;

enum { xplm_ProbeY = 0 };
typedef int XPLMProbeType;

enum {
    xplm_ProbeHitTerrain = 0,
    xplm_ProbeError      = 1,
    xplm_ProbeMissed     = 2
};
typedef int XPLMProbeResult;

typedef struct {
    int   structSize;
    float locationX;
    float locationY;
    float locationZ;
    float normalX;
    float normalY;
    float normalZ;
    float velocityX;
    float velocityY;
    float velocityZ;
    int   is_wet;
} XPLMProbeInfo_t;

XPLMProbeRef    XPLMCreateProbe(XPLMProbeType inProbeType);
void            XPLMDestroyProbe(XPLMProbeRef inProbe);
XPLMProbeResult XPLMProbeTerrainXYZ(XPLMProbeRef     inProbe,
                                             float            inX,
                                             float            inY,
                                             float            inZ,
                                             XPLMProbeInfo_t* outInfo);

#ifdef __cplusplus
}
#endif
#endif
