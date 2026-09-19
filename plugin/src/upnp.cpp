#include "upnp.h"

#include "net.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
   typedef int socklen_t;
#  define XR_INVALID (-1)
#  define XR_CLOSE(f) closesocket((SOCKET)(f))
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define XR_INVALID (-1)
#  define XR_CLOSE(f) ::close((int)(f))
#endif

namespace xr {
namespace upnp {
namespace {

// Set when the plugin is unloading. Every phase checks it, so quitting
// X-Plane never sits waiting on a router that has stopped answering.
std::atomic<bool> g_abort{false};

// --- tiny helpers -----------------------------------------------------------

std::string lower(std::string v) {
    for (auto& c : v) c = (char)tolower((unsigned char)c);
    return v;
}

// Text between <tag> and </tag>, namespace prefixes ignored. Enough for the
// three values we need out of a router's XML; a real parser would be a
// dependency for no benefit.
std::string tagValue(const std::string& xml, const std::string& tag, size_t from = 0) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    size_t a = xml.find(open, from);
    if (a == std::string::npos) return "";
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::string::npos) return "";
    std::string v = xml.substr(a, b - a);
    size_t s = v.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return "";
    size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(s, e - s + 1);
}

struct Url {
    std::string host;
    uint16_t    port = 80;
    std::string path = "/";
    bool        ok = false;
};

Url parseUrl(const std::string& url) {
    Url u;
    const std::string pre = "http://";
    if (lower(url).compare(0, pre.size(), pre) != 0) return u;
    const size_t hostStart = pre.size();
    const size_t slash = url.find('/', hostStart);
    std::string hostPort = url.substr(hostStart,
                                      slash == std::string::npos ? std::string::npos
                                                                 : slash - hostStart);
    u.path = (slash == std::string::npos) ? "/" : url.substr(slash);
    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        const int p = atoi(hostPort.c_str() + colon + 1);
        if (p > 0 && p < 65536) u.port = (uint16_t)p;
        hostPort = hostPort.substr(0, colon);
    }
    u.host = hostPort;
    u.ok = !u.host.empty();
    return u;
}

// A control URL may be absolute or a path; make it absolute against the base.
std::string resolveUrl(const Url& base, const std::string& ref) {
    if (lower(ref).compare(0, 7, "http://") == 0) return ref;
    std::string path;
    if (!ref.empty() && ref[0] == '/') {
        path = ref;
    } else {
        // relative to the directory the base document lives in
        const size_t slash = base.path.rfind('/');
        path = (slash == std::string::npos ? std::string("/") : base.path.substr(0, slash + 1)) + ref;
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "http://%s:%u%s", base.host.c_str(), (unsigned)base.port,
             path.c_str());
    return buf;
}

// --- a minimal blocking HTTP/1.1 client ------------------------------------
// Routers answer in one small response and close; this is all that needs.
bool httpRequest(const Url& u, const std::string& request, std::string* out,
                 int timeoutMs) {
    if (!u.ok) return false;

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)u.port);

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(u.host.c_str(), portStr, &hints, &res) != 0 || !res) return false;

    long long f = (long long)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (f == XR_INVALID) { freeaddrinfo(res); return false; }

    // A write to a socket the far end has closed raises SIGPIPE on POSIX,
    // and the default disposition takes the whole of X-Plane down with it.
#if defined(SO_NOSIGPIPE)
    { int one = 1; setsockopt((int)f, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
#if defined(MSG_NOSIGNAL)
    const int sendFlags = MSG_NOSIGNAL;
#else
    const int sendFlags = 0;
#endif

    // Non-blocking connect with a deadline: a router that silently drops the
    // SYN must not hold the thread for the OS default of ~2 minutes.
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)f, FIONBIO, &nb);
#else
    const int fl = fcntl((int)f, F_GETFL, 0);
    fcntl((int)f, F_SETFL, fl | O_NONBLOCK);
#endif
    const int rc = connect(
#ifdef _WIN32
        (SOCKET)f,
#else
        (int)f,
#endif
        res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);

    auto waitReady = [&](bool forWrite) {
        fd_set s;
        FD_ZERO(&s);
#ifdef _WIN32
        FD_SET((SOCKET)f, &s);
        const int nfds = 0;
#else
        FD_SET((int)f, &s);
        const int nfds = (int)f + 1;
#endif
        timeval tv;
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        return select(nfds, forWrite ? nullptr : &s, forWrite ? &s : nullptr,
                      nullptr, &tv) > 0;
    };

    if (rc != 0) {
        if (!waitReady(true)) { XR_CLOSE(f); return false; }
        // select() also reports "writable" for a connect that FAILED: only
        // SO_ERROR says which it was. Sending on a refused connection is
        // what produces the SIGPIPE / endless-retry mentioned above.
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        getsockopt(
#ifdef _WIN32
            (SOCKET)f, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl);
#else
            (int)f, SOL_SOCKET, SO_ERROR, &soerr, &sl);
#endif
        if (soerr != 0) { XR_CLOSE(f); return false; }
    }

    size_t off = 0;
    int stalls = 0;
    while (off < request.size()) {
        const int n = (int)::send(
#ifdef _WIN32
            (SOCKET)f, request.data() + off, (int)(request.size() - off), sendFlags);
#else
            (int)f, request.data() + off, request.size() - off, sendFlags);
#endif
        if (n > 0) { off += (size_t)n; continue; }
        if (n == 0 || ++stalls > 4 || g_abort.load() || !waitReady(true)) {
            XR_CLOSE(f);
            return false;
        }
    }

    std::string body;
    char buf[2048];
    for (;;) {
        if (g_abort.load() || !waitReady(false)) break;
        const int n = (int)::recv(
#ifdef _WIN32
            (SOCKET)f, buf, (int)sizeof(buf), 0);
#else
            (int)f, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;
        body.append(buf, (size_t)n);
        if (body.size() > 256 * 1024) break;      // a router that will not stop
    }
    XR_CLOSE(f);
    if (out) *out = body;
    return !body.empty();
}

std::string httpBody(const std::string& response) {
    const size_t sep = response.find("\r\n\r\n");
    if (sep == std::string::npos) return response;
    const std::string head = lower(response.substr(0, sep));
    std::string body = response.substr(sep + 4);
    if (head.find("transfer-encoding: chunked") == std::string::npos) return body;

    // De-chunk: <hex length>\r\n<bytes>\r\n ... 0\r\n
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        const size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos) break;
        const unsigned long len = strtoul(body.c_str() + pos, nullptr, 16);
        if (len == 0) break;
        pos = eol + 2;
        if (pos + len > body.size()) break;
        out.append(body, pos, len);
        pos += len + 2;
    }
    return out;
}

bool httpOk(const std::string& response) {
    return response.compare(0, 7, "HTTP/1.") == 0 &&
           response.find(" 200 ") != std::string::npos;
}

// --- the default gateway ---------------------------------------------------
// Where the internet route goes. Used to talk to the router directly when
// multicast does not reach it -- which on a PC with VirtualBox, VMware,
// Hyper-V or a VPN adapter installed is more often than not, because the
// multicast goes out of the wrong interface.
std::string gatewayAddress() {
    // Test seam: point discovery at a fake router (tools/harness/fake_igd.py).
    if (const char* forced = getenv("XRADIO_UPNP_GATEWAY")) return forced;
#ifdef _WIN32
    ULONG size = 0;
    if (GetIpForwardTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) return "";
    std::vector<unsigned char> raw(size);
    MIB_IPFORWARDTABLE* t = (MIB_IPFORWARDTABLE*)raw.data();
    if (GetIpForwardTable(t, &size, FALSE) != NO_ERROR) return "";
    DWORD best = 0xFFFFFFFFu;
    std::string out;
    for (DWORD i = 0; i < t->dwNumEntries; ++i) {
        const MIB_IPFORWARDROW& r = t->table[i];
        if (r.dwForwardDest != 0 || r.dwForwardMask != 0) continue;   // 0.0.0.0/0 only
        if (r.dwForwardMetric1 < best) {
            best = r.dwForwardMetric1;
            in_addr a;
            a.s_addr = r.dwForwardNextHop;
            char buf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &a, buf, sizeof(buf))) out = buf;
        }
    }
    return out;
#elif defined(__linux__)
    FILE* f = fopen("/proc/net/route", "r");
    if (!f) return "";
    char line[256];
    std::string out;
    while (fgets(line, sizeof(line), f)) {
        char iface[32];
        unsigned long dest = 0, gw = 0;
        if (sscanf(line, "%31s %lx %lx", iface, &dest, &gw) != 3) continue;
        if (dest != 0 || gw == 0) continue;
        in_addr a;
        a.s_addr = (uint32_t)gw;             // the file is little-endian on x86
        char buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &a, buf, sizeof(buf))) { out = buf; break; }
    }
    fclose(f);
    return out;
#else
    return "";                                // macOS: multicast has to do
#endif
}

// --- SSDP: find the gateway ------------------------------------------------
std::vector<std::string> discover(int timeoutMs) {
    std::vector<std::string> locations;

    long long f = (long long)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (f == XR_INVALID) return locations;

    // Send from the interface that carries the internet route, not whatever
    // the OS picks as its multicast default. On a machine with virtual
    // adapters those are different interfaces and the router never hears us.
    const std::string local = localAddress();
    in_addr localIn{};
    if (!local.empty() && inet_pton(AF_INET, local.c_str(), &localIn) == 1) {
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr   = localIn;
        me.sin_port   = 0;
        bind(
#ifdef _WIN32
            (SOCKET)f,
#else
            (int)f,
#endif
            (const sockaddr*)&me, sizeof(me));
        setsockopt(
#ifdef _WIN32
            (SOCKET)f, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&localIn, sizeof(localIn));
#else
            (int)f, IPPROTO_IP, IP_MULTICAST_IF, &localIn, sizeof(localIn));
#endif
    }
    unsigned char ttl = 2;
#ifdef _WIN32
    setsockopt((SOCKET)f, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
#else
    setsockopt((int)f, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
#endif

    sockaddr_in mcast{};
    mcast.sin_family = AF_INET;
    mcast.sin_port   = htons(1900);
    mcast.sin_addr.s_addr = inet_addr("239.255.255.250");

    // Also straight at the gateway: a unicast M-SEARCH is answered by every
    // router firmware that matters, and it does not care about multicast
    // routing at all.
    sockaddr_in gw{};
    bool haveGw = false;
    const std::string gwAddr = gatewayAddress();
    if (!gwAddr.empty() && inet_pton(AF_INET, gwAddr.c_str(), &gw.sin_addr) == 1) {
        gw.sin_family = AF_INET;
        gw.sin_port   = htons(1900);
        haveGw = true;
    }

    // Ask for the gateway device and for each service type directly, v1 and
    // v2: some routers only answer one of them.
    const char* targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    auto search = [&] {
        for (const char* st : targets) {
            char req[512];
            const int n = snprintf(req, sizeof(req),
                "M-SEARCH * HTTP/1.1\r\n"
                "HOST: 239.255.255.250:1900\r\n"
                "MAN: \"ssdp:discover\"\r\n"
                "MX: 1\r\n"
                "ST: %s\r\n\r\n", st);
            sendto(
#ifdef _WIN32
                (SOCKET)f, req, n, 0,
#else
                (int)f, req, (size_t)n, 0,
#endif
                (const sockaddr*)&mcast, sizeof(mcast));
            if (haveGw) {
                sendto(
#ifdef _WIN32
                    (SOCKET)f, req, n, 0,
#else
                    (int)f, req, (size_t)n, 0,
#endif
                    (const sockaddr*)&gw, sizeof(gw));
            }
        }
    };
    search();

    // MX: 1 lets the router delay its answer up to a second, so wait longer
    // than that however many replies come in, and ask again half-way in case
    // the first one was lost.
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsedMs = [&] {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    };
    const int budget = timeoutMs < 2200 ? 2200 : timeoutMs;
    bool resent = false;
    char buf[2048];
    while (elapsedMs() < budget && !g_abort.load()) {
        if (!resent && elapsedMs() > budget / 2) { search(); resent = true; }
        fd_set rd;
        FD_ZERO(&rd);
#ifdef _WIN32
        FD_SET((SOCKET)f, &rd);
        const int nfds = 0;
#else
        FD_SET((int)f, &rd);
        const int nfds = (int)f + 1;
#endif
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        if (select(nfds, &rd, nullptr, nullptr, &tv) <= 0) continue;

        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const int n = (int)recvfrom(
#ifdef _WIN32
            (SOCKET)f, buf, (int)sizeof(buf) - 1, 0,
#else
            (int)f, buf, sizeof(buf) - 1, 0,
#endif
            (sockaddr*)&from, &fromLen);
        if (n <= 0) continue;
        buf[n] = 0;

        // LOCATION: http://192.168.1.1:5000/rootDesc.xml
        const std::string reply(buf, (size_t)n);
        const std::string low = lower(reply);
        size_t p = low.find("location:");
        if (p == std::string::npos) continue;
        p += 9;
        while (p < reply.size() && (reply[p] == ' ' || reply[p] == '\t')) ++p;
        const size_t e = reply.find_first_of("\r\n", p);
        std::string loc = reply.substr(p, e == std::string::npos ? e : e - p);
        if (loc.empty()) continue;
        bool known = false;
        for (const auto& k : locations) if (k == loc) { known = true; break; }
        if (!known) locations.push_back(loc);
        if (locations.size() >= 4) break;
    }
    XR_CLOSE(f);
    return locations;
}

// --- the WAN connection service on a gateway -------------------------------
struct Service {
    std::string controlUrl;    // absolute
    std::string type;          // WANIPConnection:1 or WANPPPConnection:1
    std::string deviceName;
};

bool findService(const std::string& location, int timeoutMs, Service* out) {
    const Url base = parseUrl(location);
    if (!base.ok) return false;

    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s:%u\r\n"
             "Connection: close\r\nUser-Agent: XRadio\r\n\r\n",
             base.path.c_str(), base.host.c_str(), (unsigned)base.port);

    std::string resp;
    if (!httpRequest(base, req, &resp, timeoutMs) || !httpOk(resp)) return false;
    const std::string xml = httpBody(resp);

    // Relative control URLs resolve against <URLBase> when the description
    // gives one, else against the description's own location.
    Url urlBase = base;
    const std::string ub = tagValue(xml, "URLBase");
    if (!ub.empty()) {
        const Url parsed = parseUrl(ub);
        if (parsed.ok) urlBase = parsed;
    }

    for (const char* want : {"urn:schemas-upnp-org:service:WANIPConnection:2",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
        const std::string marker = std::string("<serviceType>") + want + "</serviceType>";
        const size_t at = xml.find(marker);
        if (at == std::string::npos) continue;
        // controlURL sits in the same <service> block, after serviceType.
        const std::string ctl = tagValue(xml, "controlURL", at);
        if (ctl.empty()) continue;
        out->controlUrl = resolveUrl(urlBase, ctl);
        out->type       = want;
        out->deviceName = tagValue(xml, "friendlyName");
        if (out->deviceName.empty()) out->deviceName = base.host;
        return true;
    }
    return false;
}

// --- SOAP ------------------------------------------------------------------
bool soap(const Service& svc, const std::string& action, const std::string& args,
          int timeoutMs, std::string* body) {
    const Url u = parseUrl(svc.controlUrl);
    if (!u.ok) return false;

    char env[1536];
    const int envLen = snprintf(env, sizeof(env),
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
        action.c_str(), svc.type.c_str(), args.c_str(), action.c_str());
    if (envLen <= 0 || envLen >= (int)sizeof(env)) return false;

    std::string req;
    {
        char head[1024];
        snprintf(head, sizeof(head),
            "POST %s HTTP/1.1\r\nHost: %s:%u\r\n"
            "Content-Type: text/xml; charset=\"utf-8\"\r\n"
            "SOAPAction: \"%s#%s\"\r\n"
            "Content-Length: %d\r\nConnection: close\r\n"
            "User-Agent: XRadio\r\n\r\n",
            u.path.c_str(), u.host.c_str(), (unsigned)u.port,
            svc.type.c_str(), action.c_str(), envLen);
        req = head;
    }
    req.append(env, (size_t)envLen);

    std::string resp;
    if (!httpRequest(u, req, &resp, timeoutMs)) return false;
    if (body) *body = httpBody(resp);
    return httpOk(resp);
}

// Routers report failures as a SOAP fault carrying a numeric code; the
// common ones are worth translating, because "718" means nothing to a pilot.
std::string soapError(const std::string& body) {
    const std::string code = tagValue(body, "errorCode");
    if (code.empty()) return "the router refused the request";
    const int c = atoi(code.c_str());
    switch (c) {
        case 718: return "that port is already forwarded to another device";
        case 725: return "the router only allows temporary port openings";
        case 501: return "the router's port forwarding failed (error 501)";
        case 606: return "the router requires a login for port forwarding";
        default:  break;
    }
    return "the router refused the request (error " + code + ")";
}

// --- the public address, without the router's help -------------------------
// When UPnP is off the router will not tell us what the internet sees us
// as, but the hosting pilot still needs it to pass on. Two plain-HTTP
// services that answer with just the address; either will do.
bool looksLikeIPv4(const std::string& v) {
    in_addr a;
    return !v.empty() && v.size() < 16 && inet_pton(AF_INET, v.c_str(), &a) == 1;
}

// Addresses that cannot be reached from the internet: private ranges, the
// carrier-grade NAT range, link-local, and "nothing".
bool isPublicIPv4(const std::string& v) {
    in_addr a;
    if (!looksLikeIPv4(v) || inet_pton(AF_INET, v.c_str(), &a) != 1) return false;
    const uint32_t ip = ntohl(a.s_addr);
    if (ip == 0) return false;
    const uint8_t b0 = (uint8_t)(ip >> 24), b1 = (uint8_t)(ip >> 16);
    if (b0 == 10) return false;
    if (b0 == 172 && b1 >= 16 && b1 <= 31) return false;
    if (b0 == 192 && b1 == 168) return false;
    if (b0 == 100 && b1 >= 64 && b1 <= 127) return false;     // CGNAT
    if (b0 == 169 && b1 == 254) return false;
    if (b0 == 127) return false;
    return true;
}

std::string fetchPublicIp(int timeoutMs) {
    const char* hosts[] = {"checkip.amazonaws.com", "api.ipify.org"};
    for (const char* h : hosts) {
        if (g_abort.load()) break;
        Url u;
        u.host = h; u.port = 80; u.path = "/"; u.ok = true;
        char req[256];
        snprintf(req, sizeof(req),
                 "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: XRadio\r\n\r\n", h);
        std::string resp;
        if (!httpRequest(u, req, &resp, timeoutMs) || !httpOk(resp)) continue;
        std::string body = httpBody(resp);
        size_t a = body.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        size_t e = body.find_last_not_of(" \t\r\n");
        body = body.substr(a, e - a + 1);
        if (looksLikeIPv4(body)) return body;
    }
    return "";
}

// --- module state ----------------------------------------------------------
std::mutex        g_mx;
Result            g_latest;
std::atomic<bool> g_busy{false};
std::thread       g_worker;
uint16_t          g_mappedPort = 0;
Service           g_mappedVia;             // the service the mapping was made through

void joinWorker() {
    if (g_worker.joinable()) g_worker.join();
}

}  // namespace

// ---------------------------------------------------------------------------
Result addMapping(uint16_t port, const std::string& localIp,
                  const std::string& description, int timeoutMs) {
    Result r;
    r.done = true;

    if (localIp.empty()) {
        r.error = "could not work out this computer's address on the network";
        return r;
    }

    const std::vector<std::string> found = discover(timeoutMs);
    if (g_abort.load()) { r.error = "cancelled"; return r; }
    if (found.empty()) {
        r.error = "no router answered (UPnP is probably switched off on it)";
        return r;
    }

    Service svc;
    bool haveSvc = false;
    for (const auto& loc : found) {
        if (g_abort.load()) { r.error = "cancelled"; return r; }
        if (findService(loc, timeoutMs, &svc)) { haveSvc = true; break; }
    }
    if (!haveSvc) {
        r.error = "the router answered but does not offer port forwarding";
        return r;
    }
    r.router = svc.deviceName;

    // Worth having even when the mapping fails: it is the address to share.
    // A router with its WAN down, or behind a carrier's NAT, reports 0.0.0.0
    // or a private address, which would be worse than nothing to pass on.
    std::string body;
    if (g_abort.load()) { r.error = "cancelled"; return r; }
    if (soap(svc, "GetExternalIPAddress", "", timeoutMs, &body)) {
        const std::string ext = tagValue(body, "NewExternalIPAddress");
        if (isPublicIPv4(ext)) {
            r.externalIp = ext;
        } else if (looksLikeIPv4(ext)) {
            r.doubleNat = true;
        }
    }

    char args[768];
    auto tryAdd = [&](int lease) {
        snprintf(args, sizeof(args),
                 "<NewRemoteHost></NewRemoteHost>"
                 "<NewExternalPort>%u</NewExternalPort>"
                 "<NewProtocol>UDP</NewProtocol>"
                 "<NewInternalPort>%u</NewInternalPort>"
                 "<NewInternalClient>%s</NewInternalClient>"
                 "<NewEnabled>1</NewEnabled>"
                 "<NewPortMappingDescription>%s</NewPortMappingDescription>"
                 "<NewLeaseDuration>%d</NewLeaseDuration>",
                 (unsigned)port, (unsigned)port, localIp.c_str(),
                 description.c_str(), lease);
        return soap(svc, "AddPortMapping", args, timeoutMs, &body);
    };

    if (g_abort.load()) { r.error = "cancelled"; return r; }
    if (tryAdd(0)) {
        r.mapped = true;
    } else if (atoi(tagValue(body, "errorCode").c_str()) == 725 && tryAdd(3600)) {
        // "only permanent leases supported" is the opposite of what it sounds
        // like on some firmware, so a timed mapping is the fallback.
        r.mapped = true;
        r.leaseSeconds = 3600;
    } else {
        r.error = soapError(body);
    }
    if (r.mapped) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_mappedVia = svc;
    }
    return r;
}

bool removeMapping(uint16_t port, int timeoutMs) {
    // The service the mapping went through is remembered, so releasing it is
    // one request rather than a fresh discovery -- which matters at quit,
    // when there is no time for one.
    Service svc;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        svc = g_mappedVia;
    }
    if (svc.controlUrl.empty()) {
        const std::vector<std::string> found = discover(timeoutMs);
        bool haveSvc = false;
        for (const auto& loc : found) {
            if (findService(loc, timeoutMs, &svc)) { haveSvc = true; break; }
        }
        if (!haveSvc) return false;
    }

    char args[256];
    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost>"
             "<NewExternalPort>%u</NewExternalPort>"
             "<NewProtocol>UDP</NewProtocol>", (unsigned)port);
    std::string body;
    return soap(svc, "DeletePortMapping", args, timeoutMs, &body);
}

std::string publicAddress(int timeoutMs) { return fetchPublicIp(timeoutMs); }

void requestAsync(uint16_t port, const std::string& description, bool askRouter) {
    if (g_busy.exchange(true)) return;          // one at a time
    joinWorker();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_latest = Result{};
    }
    const std::string desc = description;
    g_worker = std::thread([port, desc, askRouter] {
        Result r;
        if (askRouter) {
            r = addMapping(port, localAddress(), desc);
        } else {
            r.error = "not asked";
        }
        // No public address from the router? Ask the internet, so the pilot
        // still has something to pass on once they have forwarded the port.
        if (r.externalIp.empty() && !r.doubleNat && !g_abort.load()) {
            r.externalIp = fetchPublicIp(2500);
            r.externalFromWeb = !r.externalIp.empty();
        }
        r.done = true;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_latest = r;
            g_mappedPort = r.mapped ? port : 0;
        }
        g_busy.store(false);
    });
}

void releaseAsync() {
    uint16_t port;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        port = g_mappedPort;
        g_mappedPort = 0;
        g_latest = Result{};
    }
    if (port == 0 || g_busy.exchange(true)) return;
    joinWorker();
    g_worker = std::thread([port] {
        removeMapping(port, 1500);
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_mappedVia = Service{};
        }
        g_busy.store(false);
    });
}

bool busy() { return g_busy.load(); }

Result latest() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_latest;
}

void clear() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_latest = Result{};
}

void shutdown() {
    // A release started just before this gets a moment to finish -- with the
    // control URL remembered it is one request -- before the abort flag
    // cuts anything longer short. Never more than about a second.
    for (int i = 0; i < 50 && g_busy.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    g_abort.store(true);
    joinWorker();
    g_busy.store(false);
    g_abort.store(false);
}

}  // namespace upnp
}  // namespace xr
