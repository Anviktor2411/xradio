// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
void XPLMDrawString(float* inColorRGB, int inXOffset, int inYOffset,
                    char* inChar, int* inWordWrapWidth, XPLMFontID inFontID);
void XPLMGetFontDimensions(XPLMFontID inFontID, int* outCharWidth,
                           int* outCharHeight, int* outDigitsOnly);
float XPLMMeasureString(XPLMFontID inFontID, const char* inChar, int inNumChars);

// The local <-> world conversion the terrain probe rides on. The stub uses a
// flat earth in metres; only the round trip matters to the plugin.
void XPLMWorldToLocal(double inLatitude, double inLongitude, double inAltitude,
                      double* outX, double* outY, double* outZ);
void XPLMLocalToWorld(double inX, double inY, double inZ,
                      double* outLatitude, double* outLongitude, double* outAltitude);
