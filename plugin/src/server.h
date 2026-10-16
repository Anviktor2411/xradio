// The relay server, built into the plugin.
//
// Why this exists: flying together used to mean somebody first had to rent a
// VPS, install Python and open a port. Now one pilot switches Hosting on and
// the others type their address -- the same thing every multiplayer game has
// done for twenty years.
//
// This is a line-for-line port of server/server.py. The Python server is
// still the one to run on a machine that should stay up without X-Plane
// open; both are held to the same test suite (tools/test_server_parity.py runs
// against both) and route radio by the same model (radio.h), so their
// behaviour cannot drift apart.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"   // kMaxTrafficEntries: the cap is the structs' business

namespace xr {
namespace relay {

// Tuning, identical to server.py.
constexpr double kTrafficHz        = 10.0;   // twice the clients' report rate
constexpr double kTrafficRangeNm   = 80.0;
constexpr double kSessionTimeoutS  = 15.0;
constexpr double kTxHoldS          = 0.4;
constexpr int    kMaxEntriesPerPacket = kMaxTrafficEntries;   // from protocol.h
// More aircraft in range than fit in one packet are sent in several, so the
// packet size stops deciding how many aeroplanes a pilot can see. This is the
// cap on the whole lot, nearest first -- a bound on the work one crowded
// client can make the server do, not a limit anyone will meet in a flight.
constexpr int    kMaxEntriesTotal  = 60;
// Everyone in the flight goes to everyone whenever somebody joins or leaves,
// and this often besides, so a roster lost on the way heals by itself. It is
// the list the Say field offers when a pilot types "@".
constexpr double kRosterIntervalS  = 5.0;
constexpr int    kMaxTextBytes     = 200;
constexpr int    kMaxVoiceBytes    = 512;
// v6. An injected aircraft its injector stopped refreshing is gone; one
// injector may drive as many aircraft as one client's traffic set holds; the
// ATC list goes out like the roster.
constexpr double kAiTimeoutS       = 5.0;
constexpr int    kMaxAiPerInjector = kMaxEntriesTotal;
constexpr double kAtcListIntervalS = kRosterIntervalS;

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
// `password` empty means anyone may join. `ducting` is the realism switch: the
// flight's shared weather may then bring tropospheric ducting (docs/radio.md).
bool start(uint16_t port, const std::string& password, std::string* err, bool ducting = false);
void stop();
bool running();
// The realism switch, changed while the server runs -- no restart, so nobody
// connected is thrown off for it.
void setDucting(bool on);

Status status();

}  // namespace relay
}  // namespace xr
