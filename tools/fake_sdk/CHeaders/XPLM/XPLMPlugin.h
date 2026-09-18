// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
XPLMPluginID XPLMGetMyID(void);
void XPLMGetPluginInfo(XPLMPluginID inPlugin, char* outName, char* outFilePath,
                       char* outSignature, char* outDescription);
