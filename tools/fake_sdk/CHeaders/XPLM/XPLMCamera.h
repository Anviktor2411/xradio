// Fake XPLMCamera.h for the test harness: the camera control API the Tower mode uses.
#pragma once
#include "XPLMDefs.h"

typedef struct {
    float x, y, z;
    float pitch, heading, roll;
    float zoom;
} XPLMCameraPosition_t;

enum {
    xplm_ControlCameraUntilViewChanges = 1,
    xplm_ControlCameraForever          = 2
};
typedef int XPLMCameraControlDuration;

typedef int (*XPLMCameraControl_f)(XPLMCameraPosition_t* outCameraPosition, int inIsLosingControl, void* inRefcon);

#ifdef __cplusplus
extern "C" {
#endif
void XPLMControlCamera(XPLMCameraControlDuration inHowLong, XPLMCameraControl_f inControlFunc, void* inRefcon);
void XPLMDontControlCamera(void);
int  XPLMIsCameraBeingControlled(XPLMCameraControlDuration* outCameraControlDuration);
void XPLMReadCameraPosition(XPLMCameraPosition_t* outCameraPosition);
#ifdef __cplusplus
}
#endif
