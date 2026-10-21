#include "imgui_assert.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace xr {
namespace ui2 {

namespace {
std::atomic<int> g_count{0};
ImguiAssertSink  g_sink = nullptr;
std::mutex       g_mx;
std::set<std::string> g_seen;            // each assert once in Log.txt, however often it fails
constexpr size_t kMaxLogged = 20;
}  // namespace

void setImguiAssertSink(ImguiAssertSink sink) { g_sink = sink; }
int  imguiAssertCount() { return g_count.load(); }

void imguiAssertFailed(const char* expr, const char* file, int line) {
    ++g_count;
    const char* base = file ? file : "?";
    for (const char* p = base; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    char msg[512];
    snprintf(msg, sizeof msg, "ImGui assert failed: %s (%s:%d)", expr ? expr : "?", base, line);
    if (!g_sink) {                       // a test build: every failure counts and is shown
        fprintf(stderr, "FAIL  %s\n", msg);
        return;
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_seen.size() >= kMaxLogged || !g_seen.insert(msg).second) return;
    g_sink(msg);
}

}  // namespace ui2
}  // namespace xr
