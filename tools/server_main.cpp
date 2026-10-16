// The built-in relay server as a standalone program.
//
//   ./xradio_server [port] [password] [--ducting]
//
// Two reasons this exists. It lets someone run the C++ server on a machine
// without Python, and -- more usefully -- it lets tools/test_server_parity.py
// run its whole suite against the C++ server as well as the Python one, so the
// two implementations cannot quietly drift apart.
//
// --ducting (or XRADIO_DUCTING=1) is the realism switch server.py has: the
// flight's shared weather may then bring tropospheric ducting (docs/radio.md).
#include "server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    // The switch may stand anywhere; what is left are the positional port and password.
    const char* env = getenv("XRADIO_DUCTING");
    bool ducting = env && *env && strcmp(env, "0") != 0;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ducting") == 0) ducting = true;
        else pos.push_back(argv[i]);
    }

    uint16_t port = 49100;
    if (!pos.empty()) {
        const int v = atoi(pos[0]);
        if (v < 1 || v > 65535) {
            fprintf(stderr, "port must be 1-65535\n");
            return 2;
        }
        port = (uint16_t)v;
    }

    const std::string password = pos.size() > 1 ? pos[1] : "";
    std::string err;
    if (!xr::relay::start(port, password, &err, ducting)) {
        fprintf(stderr, "cannot start: %s\n", err.c_str());
        return 1;
    }
    printf("listening on 0.0.0.0:%u%s%s\n", (unsigned)port,
           password.empty() ? "" : " (password required)",
           ducting ? " (tropospheric ducting on)" : "");
    fflush(stdout);

    // One line whenever the flight changes: who is in, and how much is
    // flowing. For a live picture of positions and frequencies, the Python
    // server's --dashboard is the one to run; this one stays a log, so it
    // can run under a service manager with its output captured.
    std::vector<std::string> lastNames;
    bool first = true;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const xr::relay::Status st = xr::relay::status();
        if (first || st.callsigns != lastNames) {
            first = false;
            lastNames = st.callsigns;
            printf("pilots: %d", st.clients);
            for (const auto& c : st.callsigns) printf("  %s", c.c_str());
            printf("   (packets in %llu, out %llu)\n",
                   (unsigned long long)st.packetsIn, (unsigned long long)st.packetsOut);
            fflush(stdout);
        }
    }
}
