// COMPILE-CHECK STUB ONLY -- not the real X-Plane SDK.
// Just enough of the API surface that main.cpp uses, so CI can catch syntax
// and signature mistakes without shipping Laminar's headers. Build the real
// plugin against the real SDK.
#pragma once

#define PLUGIN_API extern "C"

typedef int  XPLMPluginID;
typedef void* XPLMWindowID;
typedef void* XPLMMenuID;
typedef void* XPLMDataRef;
typedef void* XPLMCommandRef;
typedef void* XPLMFlightLoopID;
typedef int  XPLMKeyFlags;
typedef int  XPLMFontID;

enum { xplmFont_Basic = 0, xplmFont_Proportional = 18 };

typedef int XPLMMouseStatus;
enum { xplm_MouseDown = 1, xplm_MouseDrag = 2, xplm_MouseUp = 3 };

typedef int XPLMCursorStatus;
enum { xplm_CursorDefault = 0, xplm_CursorHidden = 1, xplm_CursorArrow = 2 };
typedef enum { xplm_CommandBegin = 0, xplm_CommandContinue, xplm_CommandEnd } XPLMCommandPhase;
