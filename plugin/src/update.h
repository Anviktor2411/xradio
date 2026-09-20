// "There is a newer XRadio" -- asked of GitHub, once per sim session.
//
// Everyone in a flight has to be on the same build, so a pilot running an old
// one does not merely miss out: they cannot join at all, and the rejection
// tells them why but not where to get the new one. This closes that gap.
//
// It is deliberately small and deliberately optional. One request at startup,
// to the repository's releases endpoint, on a worker thread; a pilot who
// would rather the sim talked to nobody can switch it off in Settings and
// nothing is sent. Nothing is downloaded or installed -- the window shows the
// version and the address, and the pilot decides.
#pragma once

#include <string>

namespace xr {
namespace update {

struct Info {
    bool        checked = false;   // the attempt finished, one way or another
    bool        newer = false;     // ...and there is something newer out there
    std::string latest;            // "0.6.0"
    std::string url;               // where to get it
    std::string error;             // why the check did not work
};

// Is `candidate` a later version than `current`? Compares numerically, so
// 0.5.10 beats 0.5.9, and anything unparseable is treated as "not newer".
bool isNewer(const std::string& current, const std::string& candidate);

// Fire and forget, once per session. `current` is what we are running.
// Does nothing if a check is already running or has finished.
void checkAsync(const std::string& current);

Info latest();
void shutdown();            // join the worker; call before unload

}  // namespace update
}  // namespace xr
