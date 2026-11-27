// The XRadio version: one place, set by tools/release.py. In a header of its own, free of the X-Plane SDK, so
// the relay server (built into the plugin and standalone as xradio_server) can tell the LAN search what it is.
#pragma once

namespace xr {
namespace brand {

inline const char* version() { return "0.7.0"; }

}  // namespace brand
}  // namespace xr
