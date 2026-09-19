// The built-in relay server as a standalone program.
//
//   ./xradio_server [port]
//
// Two reasons this exists. It lets someone run the C++ server on a machine
// without Python, and -- more usefully -- it lets tools/test_server.py run
// its whole suite against the C++ server as well as the Python one, so the
// two implementations cannot quietly drift apart.
#include "server.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    uint16_t port = 49100;
    if (argc > 1) {
        const int v = atoi(argv[1]);
        if (v < 1 || v > 65535) {
            fprintf(stderr, "port must be 1-65535\n");
            return 2;
        }
        port = (uint16_t)v;
    }

    std::string err;
    if (!xr::relay::start(port, &err)) {
        fprintf(stderr, "cannot start: %s\n", err.c_str());
        return 1;
    }
    // test_server.py waits for this line before it starts sending.
    printf("listening on 0.0.0.0:%u\n", (unsigned)port);
    fflush(stdout);

    int lastClients = -1;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const xr::relay::Status st = xr::relay::status();
        if (st.clients != lastClients) {
            lastClients = st.clients;
            printf("clients: %d\n", st.clients);
            fflush(stdout);
        }
    }
}
