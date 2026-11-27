// The built-in relay server as a standalone program.
//
//   ./xradio_server [port] [password] [--ducting] [--max-pilots N] [--hidden] [--name NAME]
//                   [--admin-token TOKEN]
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
    // The switches may stand anywhere; what is left are the positional port and password.
    // 0.7 adds the ones server.py has for whoever runs the server: --max-pilots N, --hidden,
    // --name NAME and --admin-token TOKEN (or XRADIO_MAX_PILOTS, XRADIO_HIDDEN, XRADIO_NAME,
    // XRADIO_ADMIN_TOKEN), so tools/test_server_parity.py can start both the same way.
    auto env = [](const char* name) { const char* v = getenv(name); return std::string(v ? v : ""); };
    auto on = [](const std::string& v) { return !v.empty() && v != "0"; };
    xr::relay::Options o;
    o.ducting = on(env("XRADIO_DUCTING"));
    o.maxPilots = atoi(env("XRADIO_MAX_PILOTS").c_str());
    o.visible = !on(env("XRADIO_HIDDEN"));
    o.name = env("XRADIO_NAME");
    o.adminToken = env("XRADIO_ADMIN_TOKEN");
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (strcmp(argv[i], "--ducting") == 0) o.ducting = true;
        else if (strcmp(argv[i], "--hidden") == 0) o.visible = false;
        else if (strcmp(argv[i], "--max-pilots") == 0 && more) o.maxPilots = atoi(argv[++i]);
        else if (strcmp(argv[i], "--name") == 0 && more) o.name = argv[++i];
        else if (strcmp(argv[i], "--admin-token") == 0 && more) o.adminToken = argv[++i];
        else pos.push_back(argv[i]);
    }
    if (o.maxPilots < 0) o.maxPilots = 0;

    uint16_t port = 49100;
    if (!pos.empty()) {
        const int v = atoi(pos[0]);
        if (v < 1 || v > 65535) {
            fprintf(stderr, "port must be 1-65535\n");
            return 2;
        }
        port = (uint16_t)v;
    }
    o.port = port;
    o.password = pos.size() > 1 ? pos[1] : "";
    const std::string& password = o.password;
    const bool ducting = o.ducting;
    std::string err;
    if (!xr::relay::start(o, &err)) {
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
