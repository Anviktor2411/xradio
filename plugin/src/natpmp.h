// NAT-PMP (RFC 6886): the other way to ask a router to open a port.
//
// UPnP IGD is what most consumer routers speak, but plenty of them ship with
// it switched off while a second protocol is still listening -- Apple's
// AirPort range, a lot of OpenWrt and Fritz!Box firmware, and anything
// running miniupnpd, which answers both. It is worth asking twice before
// telling a pilot to go and configure port forwarding by hand.
//
// The protocol is four UDP packets' worth of work: two bytes to ask what the
// router's external address is, twelve to ask for a mapping. That is the
// whole appeal -- no HTTP, no XML, no device description to fetch, so it
// either answers within a couple of hundred milliseconds or it is not there.
#pragma once

#include <cstdint>
#include <string>

namespace xr {
namespace natpmp {

struct Result {
    bool        answered = false;   // something spoke NAT-PMP at all
    bool        mapped = false;     // and gave us the port
    uint16_t    externalPort = 0;   // what it opened, which may not be what we asked for
    std::string externalIp;         // the router's WAN address, if it told us
    int         leaseSeconds = 0;   // how long it promised to keep it
    std::string error;              // in words a pilot can act on
};

// Ask `gateway` (the default route's router) to forward a UDP port here.
// Blocking, at most a few hundred milliseconds -- a router that does not
// speak NAT-PMP simply never answers.
Result map(const std::string& gateway, uint16_t port, int lifetimeSeconds = 3600,
           int timeoutMs = 250);

// Give the port back. Best effort: a router that has already forgotten the
// mapping, or rebooted, is not an error worth reporting.
bool unmap(const std::string& gateway, uint16_t port, int timeoutMs = 250);

}  // namespace natpmp
}  // namespace xr
