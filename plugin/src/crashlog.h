// When the plugin takes X-Plane down, Log.txt should say where. X-Plane's own line ("This application has
// crashed because of the plugin: XRadio") names the plugin and nothing else; the crash reports it writes do
// not reach a plugin author. So on Windows the plugin installs a handler that only writes -- the exception
// code, the faulting instruction as <module>+<offset>, and a short stack the same way -- and then lets the
// previous handler, X-Plane's, go on exactly as before. The offsets are resolved against the symbol file the
// build keeps beside the plugin (XRadio.xpl.debug for MinGW, XRadio.pdb for MSVC; see CMakeLists.txt).
// Nothing else is done in the handler: no allocation, no C++ exceptions, no unloading.
#pragma once

#include <string>

namespace xr {
namespace crashlog {

// Where the lines go: the plugin passes XPLMDebugString, a test a file. Each line ends with "\n".
using Sink = void (*)(const char* line);

// Install the handlers (Windows; a no-op elsewhere). Returns false where nothing could be installed.
bool install(Sink sink);
void uninstall();

// "<module>+0x<offset>" for an address, "0x<address>" when no module holds it. Safe to call from a handler.
std::string describeAddress(const void* addr);

// What the handler writes for a given fault, for the test: the lines for `code` at `addr` with no stack.
std::string describeFault(unsigned long code, const void* addr);

// A crash on purpose, for the test's child process: an access violation at a known address.
void crashNow();

}  // namespace crashlog
}  // namespace xr
