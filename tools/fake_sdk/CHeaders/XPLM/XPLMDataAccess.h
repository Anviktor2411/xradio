// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
XPLMDataRef XPLMFindDataRef(const char* inDataRefName);
float  XPLMGetDataf(XPLMDataRef);
double XPLMGetDatad(XPLMDataRef);
int    XPLMGetDatai(XPLMDataRef);
int    XPLMGetDatavf(XPLMDataRef, float* outValues, int inOffset, int inMax);
int    XPLMGetDatab(XPLMDataRef, void* outValue, int inOffset, int inMaxBytes);
void   XPLMSetDataf(XPLMDataRef, float inValue);
void   XPLMSetDatai(XPLMDataRef, int inValue);
void   XPLMSetDatavf(XPLMDataRef, float* inValues, int inOffset, int inCount);
