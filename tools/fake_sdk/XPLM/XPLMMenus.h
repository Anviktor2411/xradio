// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
typedef void (*XPLMMenuHandler_f)(void*, void*);
XPLMMenuID XPLMFindPluginsMenu(void);
int  XPLMAppendMenuItem(XPLMMenuID, const char*, void*, int);
XPLMMenuID XPLMCreateMenu(const char*, XPLMMenuID, int, XPLMMenuHandler_f, void*);
void XPLMDestroyMenu(XPLMMenuID);
