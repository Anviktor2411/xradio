// Ask the router to open the host port, so a pilot hosting a flight does not
// have to go and configure port forwarding.
//
// This speaks UPnP IGD, which is what nearly every consumer router has, and
// what games have used for this since the 2000s. It is best-effort by
// design: plenty of routers have it switched off, and carrier-grade NAT
// makes it pointless anyway. Every failure path ends in a sentence the
// hosting pilot can act on, never a hang -- it runs on its own thread with
// short timeouts and the plugin never waits for it.
#pragma once

#include <cstdint>
#include <string>

namespace xr {
namespace upnp {

struct Result {
    bool        done = false;       // the attempt has finished
    bool        mapped = false;     // the router opened the port
    std::string externalIp;         // what the internet sees us as, if known
    bool        externalFromWeb = false;   // ...learned from the internet, not the router
    bool        doubleNat = false;  // the router's own WAN address is private: CGNAT /
                                    // double NAT, so opening its port cannot help
    int         leaseSeconds = 0;   // 0 = permanent; otherwise the router insisted on a lease
    std::string router;             // which device answered
    std::string error;              // why it did not work, in plain words
};

// Blocking, a few seconds at worst. Used by the async helpers below and
// directly by the tests.
Result addMapping(uint16_t port, const std::string& localIp,
                  const std::string& description, int timeoutMs = 1500);
bool   removeMapping(uint16_t port, int timeoutMs = 1500);

// The address the internet sees this machine as, asked of the internet
// itself. Empty if offline. Blocking; the async helper calls it for you.
std::string publicAddress(int timeoutMs = 2500);

// Fire and forget from the main thread: kicks off a worker, leaves the
// answer in latest(). Calling it again while one is running is a no-op.
// With askRouter false only the public address is looked up (for the
// "forward the port yourself" instructions); the router is left alone.
void   requestAsync(uint16_t port, const std::string& description, bool askRouter);
void   releaseAsync();          // remove whatever we mapped
bool   busy();
Result latest();
void   clear();
void   shutdown();              // join any worker; call before unload

}  // namespace upnp
}  // namespace xr
