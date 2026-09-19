// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
void XPLMDrawString(float* inColorRGB, int inXOffset, int inYOffset,
                    char* inChar, int* inWordWrapWidth, XPLMFontID inFontID);
void XPLMGetFontDimensions(XPLMFontID inFontID, int* outCharWidth,
                           int* outCharHeight, int* outDigitsOnly);
