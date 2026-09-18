// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
XPLMDataRef XPLMFindDataRef(const char* inDataRefName);
float  XPLMGetDataf(XPLMDataRef);
double XPLMGetDatad(XPLMDataRef);
int    XPLMGetDatai(XPLMDataRef);
int    XPLMGetDatavf(XPLMDataRef, float* outValues, int inOffset, int inMax);
