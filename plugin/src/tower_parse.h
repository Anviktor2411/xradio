// The trainer's "airport" and "state" lines into tower::State -- the part of Tower mode that is pure data, with
// nothing from X-Plane, so tools/ui_preview and tools/harness/tower_ui_test load fixtures with the same code the
// plugin runs on every line (tower.cpp adds the log line, the 3D traffic's samples and the timestamps).
#pragma once

#include <string>
#include <vector>

#include "json.h"
#include "tower.h"

namespace xr {
namespace tower {

void applyAirport(State& s, const json::Value& m);
// `position` is the one claimed in the settings: whose log to show until the trainer confirms a claim.
void applyState(State& s, const json::Value& m, const std::string& position);
// What a trainer without the "delivery" field gets: the same requests, the clearance put together here.
std::vector<DelRequest> composeDelivery(const std::vector<Aircraft>& ac);

// The same from a line of text (fixtures). False when it is not that kind of line.
bool parseAirportLine(const std::string& line, State& s);
bool parseStateLine(const std::string& line, State& s, const std::string& position);

}  // namespace tower
}  // namespace xr
