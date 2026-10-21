// Where a failed IM_ASSERT goes (see xr_imconfig.h). Nothing here aborts.
#pragma once

namespace xr {
namespace ui2 {

// The plugin hands in a function that writes to Log.txt; with none set (the test builds), each failure is
// printed as a FAIL line on stderr.
using ImguiAssertSink = void (*)(const char* message);
void setImguiAssertSink(ImguiAssertSink sink);
int  imguiAssertCount();                    // failures so far: a test fails unless it stays 0
void imguiAssertFailed(const char* expr, const char* file, int line);

}  // namespace ui2
}  // namespace xr
