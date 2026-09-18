// Pi, portably.
//
// M_PI is not standard C++: it comes from POSIX, and MSVC only defines it in
// <cmath> when _USE_MATH_DEFINES was defined *before* the include. That makes
// every use of M_PI depend on include order, which is how the Windows CI
// build broke while Linux and macOS were happily green. Use xr::kPi instead;
// tools/check_portability.sh fails the build if M_PI creeps back in.
#pragma once

namespace xr {

inline constexpr double kPi = 3.14159265358979323846;

// Degrees <-> radians, for the geodesy dotted around the plugin.
inline constexpr double degToRad(double deg) { return deg * kPi / 180.0; }

}  // namespace xr
