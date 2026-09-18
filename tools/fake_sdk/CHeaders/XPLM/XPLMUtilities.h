// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
void XPLMDebugString(const char*);
void XPLMGetPrefsPath(char* outPrefsPath);
const char* XPLMGetDirectorySeparator(void);
void XPLMExtractFileAndPath(char* inFullPath);
int  XPLMEnableFeature(const char*, int);
XPLMCommandRef XPLMCreateCommand(const char*, const char*);
void XPLMRegisterCommandHandler(XPLMCommandRef, int (*)(XPLMCommandRef, XPLMCommandPhase, void*), int, void*);
void XPLMUnregisterCommandHandler(XPLMCommandRef, int (*)(XPLMCommandRef, XPLMCommandPhase, void*), int, void*);
