// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
typedef float (*XPLMFlightLoop_f)(float, float, int, void*);
typedef enum {
    xplm_FlightLoop_Phase_BeforeFlightModel = 0,
    xplm_FlightLoop_Phase_AfterFlightModel  = 1
} XPLMFlightLoopPhaseType;
typedef struct {
    int structSize;
    XPLMFlightLoopPhaseType phase;
    XPLMFlightLoop_f callbackFunc;
    void* refcon;
} XPLMCreateFlightLoop_t;
XPLMFlightLoopID XPLMCreateFlightLoop(XPLMCreateFlightLoop_t*);
void XPLMDestroyFlightLoop(XPLMFlightLoopID);
void XPLMScheduleFlightLoop(XPLMFlightLoopID, float inInterval, int inRelativeToNow);
