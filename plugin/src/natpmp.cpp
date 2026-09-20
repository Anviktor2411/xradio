#include "natpmp.h"

#include "net.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef int socklen_t;
#  define XR_CLOSE(f) closesocket((SOCKET)(f))
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define XR_CLOSE(f) ::close((int)(f))
#endif

namespace xr {
namespace natpmp {

namespace {

const uint16_t kPort = 5351;        // the router listens here
const uint8_t  kVersion = 0;
const uint8_t  kOpExternal = 0;     // "what is your WAN address?"
const uint8_t  kOpMapUdp = 1;       // "forward a UDP port to me"
const uint8_t  kOpResponse = 128;   // added to the opcode in replies

void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
uint16_t get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// The RFC's result codes, as something a pilot can read.
const char* resultText(uint16_t code) {
    switch (code) {
        case 0: return "";
        case 1: return "the router speaks a newer version of NAT-PMP";
        case 2: return "the router refused (port mapping is switched off on it)";
        case 3: return "the router has no internet connection";
        case 4: return "the router is out of resources for port mappings";
        case 5: return "the router does not support this kind of mapping";
        default: return "the router refused, without saying why";
    }
}

// One request, one reply. Returns the number of bytes received, or 0.
// Deliberately short: a router that does not speak NAT-PMP says nothing at
// all, and the pilot is waiting for the hosting window to settle.
int ask(const std::string& gateway, const uint8_t* req, int reqLen,
        uint8_t* reply, int replyMax, int timeoutMs, int attempts = 2) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port   = htons(kPort);
    if (inet_pton(AF_INET, gateway.c_str(), &to.sin_addr) != 1) return 0;

    const long long f = (long long)socket(AF_INET, SOCK_DGRAM, 0);
    if (f < 0) return 0;

    int got = 0;
    // The RFC's first retransmit interval is 250 ms, which is already
    // generous for a packet that crosses the living room. The whole budget
    // matters: the hosting window is waiting on this before it can tell the
    // pilot anything.
    for (int attempt = 0; attempt < attempts && got == 0; ++attempt) {
        if (sendto((int)f, (const char*)req, (size_t)reqLen, 0,
                   (const sockaddr*)&to, sizeof(to)) < 0) break;
        if (waitSocket(f, false, timeoutMs) <= 0) continue;

        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const int n = (int)recvfrom((int)f, (char*)reply, (size_t)replyMax, 0,
                                    (sockaddr*)&from, &fromLen);
        // Only the router we asked gets to answer for it.
        if (n > 0 && from.sin_addr.s_addr == to.sin_addr.s_addr) got = n;
    }
    XR_CLOSE(f);
    return got;
}

}  // namespace

Result map(const std::string& gateway, uint16_t port, int lifetimeSeconds,
           int timeoutMs) {
    Result r;
    if (gateway.empty() || port == 0) {
        r.error = "no router address to ask";
        return r;
    }

    // First the WAN address: it is the one thing worth having even if the
    // mapping itself is refused, and it doubles as "is anyone there?".
    uint8_t req[12] = {kVersion, kOpExternal};
    uint8_t reply[32] = {0};
    int n = ask(gateway, req, 2, reply, sizeof(reply), timeoutMs);
    const bool silent = (n == 0);
    if (n >= 12 && reply[0] == kVersion && reply[1] == (kOpResponse + kOpExternal)) {
        r.answered = true;
        const uint16_t code = get16(reply + 2);
        if (code == 0) {
            char ip[INET_ADDRSTRLEN] = {0};
            in_addr a{};
            a.s_addr = htonl(get32(reply + 8));
            if (inet_ntop(AF_INET, &a, ip, sizeof(ip))) r.externalIp = ip;
        } else {
            r.error = resultText(code);
        }
    }

    // Then the mapping. Asking for the same number on the outside is only a
    // suggestion; the router may hand back a different one, and a pilot
    // needs to be told which, so it is reported rather than assumed.
    memset(req, 0, sizeof(req));
    req[0] = kVersion;
    req[1] = kOpMapUdp;
    put16(req + 4, port);                                  // internal port
    put16(req + 6, port);                                  // suggested external
    put32(req + 8, (uint32_t)(lifetimeSeconds < 0 ? 0 : lifetimeSeconds));

    memset(reply, 0, sizeof(reply));
    // Nothing answered the first question, so ask the second one once
    // rather than twice: a router that speaks NAT-PMP answers both.
    n = ask(gateway, req, 12, reply, sizeof(reply), timeoutMs, silent ? 1 : 2);
    if (n < 16 || reply[0] != kVersion || reply[1] != (kOpResponse + kOpMapUdp)) {
        if (!r.answered) r.error = "no answer to NAT-PMP either";
        else if (r.error.empty()) r.error = "the router ignored the port request";
        return r;
    }

    r.answered = true;
    const uint16_t code = get16(reply + 2);
    if (code != 0) {
        r.error = resultText(code);
        return r;
    }
    r.externalPort = get16(reply + 10);
    r.leaseSeconds = (int)get32(reply + 12);
    // A router that opens a different external port has not given us what a
    // join code or a shared address would point at, so that is a failure
    // here even though the protocol calls it success.
    if (r.externalPort != port) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "the router opened port %u instead of %u, so it cannot be shared",
                 (unsigned)r.externalPort, (unsigned)port);
        r.error = msg;
        return r;
    }
    r.mapped = true;
    r.error.clear();
    return r;
}

bool unmap(const std::string& gateway, uint16_t port, int timeoutMs) {
    if (gateway.empty() || port == 0) return false;
    // A lifetime of zero with no suggested external port is how the RFC
    // spells "forget this mapping".
    uint8_t req[12] = {0};
    req[0] = kVersion;
    req[1] = kOpMapUdp;
    put16(req + 4, port);
    put16(req + 6, 0);
    put32(req + 8, 0);

    uint8_t reply[32] = {0};
    const int n = ask(gateway, req, 12, reply, sizeof(reply), timeoutMs);
    return n >= 16 && reply[0] == kVersion &&
           reply[1] == (kOpResponse + kOpMapUdp) && get16(reply + 2) == 0;
}

}  // namespace natpmp
}  // namespace xr
