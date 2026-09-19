// The relay server, built into the plugin.
//
// Why this exists: flying together used to mean somebody first had to rent a
// VPS, install Python and open a port. Now one pilot switches Hosting on and
// the others type their address -- the same thing every multiplayer game has
// done for twenty years.
//
// This is a line-for-line port of server/server.py. The Python server is
// still the one to run on a machine that should stay up without X-Plane
// open; both are held to the same test suite (tools/test_server.py runs
// against either), so their behaviour cannot drift apart.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xr {
namespace relay {

// Tuning, identical to server.py.
constexpr double kTrafficHz        = 10.0;   // twice the clients' report rate
constexpr double kTrafficRangeNm   = 80.0;
constexpr double kSessionTimeoutS  = 15.0;
constexpr double kTxHoldS          = 0.4;
constexpr int    kMaxEntriesPerPacket = 15;  // 15 * 88 + 16 < 1400 bytes
constexpr int    kMaxTextBytes     = 200;
constexpr int    kMaxVoiceBytes    = 512;

struct Status {
    bool        running = false;
    uint16_t    port = 0;
    int         clients = 0;         // sessions currently logged in
    uint64_t    packetsIn = 0;
    uint64_t    packetsOut = 0;
    std::string error;               // why it is not running
    std::vector<std::string> callsigns;   // who is connected, for the window
};

// Starts the listener and its own thread. Returns false (and fills `err`) if
// the port cannot be bound -- almost always something else already on it.
bool start(uint16_t port, std::string* err);
void stop();
bool running();

Status status();

}  // namespace relay
}  // namespace xr
