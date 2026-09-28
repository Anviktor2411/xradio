// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
#pragma once
#include "XPLMDefs.h"
typedef void (*XPLMDrawWindow_f)(XPLMWindowID, void*);
typedef int  (*XPLMHandleMouseClick_f)(XPLMWindowID, int, int, XPLMMouseStatus, void*);
typedef void (*XPLMHandleKey_f)(XPLMWindowID, char, XPLMKeyFlags, char, void*, int);
typedef XPLMCursorStatus (*XPLMHandleCursor_f)(XPLMWindowID, int, int, void*);
typedef int  (*XPLMHandleMouseWheel_f)(XPLMWindowID, int, int, int, int, void*);
typedef enum { xplm_WindowLayerFloatingWindows = 0 } XPLMWindowLayer;
typedef enum {
    xplm_WindowDecorationNone = 0,
    xplm_WindowDecorationRoundRectangle = 1
} XPLMWindowDecoration;
typedef struct {
    int structSize;
    int left, top, right, bottom;
    int visible;
    XPLMDrawWindow_f drawWindowFunc;
    XPLMHandleMouseClick_f handleMouseClickFunc;
    XPLMHandleKey_f handleKeyFunc;
    XPLMHandleCursor_f handleCursorFunc;
    XPLMHandleMouseWheel_f handleMouseWheelFunc;
    void* refcon;
    XPLMWindowDecoration decorateAsFloatingWindow;
    XPLMWindowLayer layer;
    XPLMHandleMouseClick_f handleRightClickFunc;
} XPLMCreateWindow_t;
typedef int (*XPLMKeySniffer_f)(char inChar, XPLMKeyFlags inFlags, char inVirtualKey, void* inRefcon);
int XPLMRegisterKeySniffer(XPLMKeySniffer_f, int inBeforeWindows, void* inRefcon);
int XPLMUnregisterKeySniffer(XPLMKeySniffer_f, int inBeforeWindows, void* inRefcon);
XPLMWindowID XPLMCreateWindowEx(XPLMCreateWindow_t*);
void XPLMDestroyWindow(XPLMWindowID);
void XPLMGetWindowGeometry(XPLMWindowID, int*, int*, int*, int*);
void XPLMSetWindowTitle(XPLMWindowID, const char*);
void XPLMSetWindowResizingLimits(XPLMWindowID, int, int, int, int);
void XPLMSetWindowIsVisible(XPLMWindowID, int);
int  XPLMGetWindowIsVisible(XPLMWindowID);
void XPLMGetScreenBoundsGlobal(int*, int*, int*, int*);
void XPLMBringWindowToFront(XPLMWindowID);
void XPLMSetWindowGeometry(XPLMWindowID, int, int, int, int);
typedef void (*XPLMReceiveMonitorBoundsGlobal_f)(int, int, int, int, int, void*);
void XPLMGetAllMonitorBoundsGlobal(XPLMReceiveMonitorBoundsGlobal_f, void*);
void XPLMTakeKeyboardFocus(XPLMWindowID);
int  XPLMHasKeyboardFocus(XPLMWindowID);
typedef int (*XPLMCommandCallback_f)(XPLMCommandRef, XPLMCommandPhase, void*);
