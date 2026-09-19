// The UPnP client against a fake router (tools/harness/fake_igd.py), in each
// of its moods. Real routers are the only other way to run this code, and
// every one of them is different; the fake does the awkward things several
// of them do -- chunked description, relative control URL through URLBase,
// "timed leases only" -- so the client is known to cope before it meets one.
//
//   XRADIO_UPNP_GATEWAY=127.0.0.1 ./upnp_test <mode>
//
// The fake must already be running in that mode.
#include "upnp.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "ok";
    printf("\nfake router in mode '%s'\n", mode.c_str());

    const auto t0 = std::chrono::steady_clock::now();
    xr::upnp::Result r = xr::upnp::addMapping(49100, "127.0.0.1", "XRadio test", 1500);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  (took %.1f s)  mapped=%d ext='%s' router='%s' lease=%d doubleNat=%d error='%s'\n",
           secs, (int)r.mapped, r.externalIp.c_str(), r.router.c_str(), r.leaseSeconds,
           (int)r.doubleNat, r.error.c_str());

    if (mode == "ok" || mode == "lease") {
        check("the router was found by name", r.router == "Fake Router", r.router);
        check("the port was opened", r.mapped, r.error);
        check("the public address came back", r.externalIp == "203.0.113.7", r.externalIp);
        check("not flagged as double NAT", !r.doubleNat);
        if (mode == "lease") {
            check("fell back to a timed lease after error 725", r.leaseSeconds == 3600,
                  std::to_string(r.leaseSeconds));
        } else {
            check("a permanent mapping was accepted", r.leaseSeconds == 0);
        }
        check("the mapping can be removed again", xr::upnp::removeMapping(49100, 1500));
        check("removing it twice fails honestly", !xr::upnp::removeMapping(49100, 1500));
    } else if (mode == "refuse") {
        check("the router was found", r.router == "Fake Router", r.router);
        check("the refusal is reported", !r.mapped);
        check("...in words a pilot can act on",
              r.error.find("already forwarded to another device") != std::string::npos, r.error);
        check("the public address still came back", r.externalIp == "203.0.113.7", r.externalIp);
    } else if (mode == "private-wan") {
        check("the mapping itself succeeds", r.mapped);
        check("but a private WAN address is recognised as double NAT", r.doubleNat);
        check("and is not offered as something to share", r.externalIp.empty(), r.externalIp);
    } else if (mode == "deaf") {
        check("nobody answered", !r.mapped && r.router.empty());
        check("it says so", r.error.find("no router answered") != std::string::npos, r.error);
        check("and gave up within four seconds", secs < 4.0, std::to_string(secs));
    }

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all upnp tests passed (%s)\n", mode.c_str());
    return 0;
}
