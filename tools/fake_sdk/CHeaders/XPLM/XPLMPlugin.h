// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
XPLMPluginID XPLMGetMyID(void);
void XPLMGetPluginInfo(XPLMPluginID inPlugin, char* outName, char* outFilePath,
                       char* outSignature, char* outDescription);
#define XPLM_MSG_PLANE_LOADED 102
#define XPLM_MSG_LIVERY_LOADED 108
