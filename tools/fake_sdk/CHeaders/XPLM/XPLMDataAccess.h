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

// Tower mode: doubles and int arrays (the user's position, override_planepath)
void   XPLMSetDatad(XPLMDataRef, double);
int    XPLMGetDatavi(XPLMDataRef, int* outValues, int inOffset, int inMax);
void   XPLMSetDatavi(XPLMDataRef, int* inValues, int inOffset, int inCount);
