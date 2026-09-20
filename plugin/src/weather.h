// The flight's shared sky: one pilot's weather and clock, applied by the rest.
//
// Flying together in different weather is worse than it sounds -- one pilot
// breaks out of cloud at 600 ft while another is in sunshine, the winds
// disagree so the aircraft that is holding station drifts away, and a
// straight-in approach happens in daylight for one and at night for the
// other. X-Plane's own weather is per-machine, so somebody has to be the
// authority; here it is whoever hosts the flight.
//
// What travels is X-Plane 12's region weather whole (three cloud layers,
// thirteen altitude levels of wind, temperature and turbulence) plus zulu
// time and the date -- not a summary of them, so nothing is quietly lost in
// translation. Reading and writing are separate so the test harness can
// drive either side without a sim.
#pragma once

#include <string>

#include "protocol.h"

namespace xr {
namespace weather {

// Look up the region weather and time datarefs. Names that X-Plane does not
// know are logged once and then treated as zero, as everywhere else.
void init();

// True when the datarefs exist at all, i.e. this is X-Plane 12.
bool available();

// Fill `out` from the sim. Returns false if there is nothing to read.
bool read(WeatherPayload& out);

// Apply a received sky. The clock is only moved when it has really drifted
// (`snapSeconds`), because writing zulu time every packet fights the sim's
// own clock and shows up as a stuttering sun.
void apply(const WeatherPayload& w, float snapSeconds = 30.f);

// What the window shows: "" when nothing has been applied yet.
std::string lastApplied();

}  // namespace weather
}  // namespace xr
