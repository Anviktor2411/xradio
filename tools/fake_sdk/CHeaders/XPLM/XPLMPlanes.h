// COMPILE-CHECK STUB ONLY -- see XPLMDefs.h
// The one call the plugin makes: which .acf the user flies. The stub in
// xplm_stub.cpp answers with the harness string "harness/acf_path".
#pragma once
#include "XPLMDefs.h"
#ifdef __cplusplus
extern "C" {
#endif
void XPLMGetNthAircraftModel(int inIndex, char* outFileName, char* outPath);
#ifdef __cplusplus
}
#endif
