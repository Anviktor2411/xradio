// Dear ImGui's configuration for XRadio: lib/imgui reads it through IMGUI_USER_CONFIG (CMakeLists.txt sets it
// for every target that compiles ImGui).
//
// IM_ASSERT never aborts. In the plugin a failed assert is written to X-Plane's Log.txt (the first few, each
// once) and the window carries on -- an assert that ends the sim is worse than one that leaves a line saying
// where the window went wrong. The test builds count every failure and fail on it (imgui_assert.h).
#pragma once

namespace xr {
namespace ui2 {
void imguiAssertFailed(const char* expr, const char* file, int line);
}  // namespace ui2
}  // namespace xr

#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : ::xr::ui2::imguiAssertFailed(#_EXPR, __FILE__, __LINE__))
