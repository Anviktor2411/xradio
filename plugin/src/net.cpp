#include "net.h"

#include <cstdio>    // snprintf -- MSVC does not pull this in via <cstring>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   // SIO_UDP_CONNRESET lives here, not in winsock2.h. Leaving it out compiles
   // fine everywhere except MSVC, which is exactly the kind of mistake
   // tools/check_portability.sh now looks for.
#  include <mstcpip.h>
   typedef int socklen_t;
#  define XR_INVALID (-1)
#  define XR_CLOSE(f) closesocket((SOCKET)(f))
#  define XR_WOULDBLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define XR_INVALID (-1)
#  define XR_CLOSE(f) ::close((int)(f))
#  define XR_WOULDBLOCK (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

namespace xr {

// Wait for a socket to become readable or writable. select() is used on
// Windows, where its fd_set is a list; on POSIX it is a bitmap of at most
// FD_SETSIZE (1024) descriptors, and X-Plane with a few hundred scenery
// files and other plugins' sockets open can hand us a descriptor above
// that -- FD_SET then writes past the end of the set. poll() has no limit.
int waitSocket(long long fd, bool forWrite, int timeoutMs) {
    if (fd == XR_INVALID) return -1;
#ifdef _WIN32
    fd_set s;
    FD_ZERO(&s);
    FD_SET((SOCKET)fd, &s);
    timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(0, forWrite ? nullptr : &s, forWrite ? &s : nullptr, nullptr, &tv);
#else
    pollfd p{};
    p.fd = (int)fd;
    p.events = forWrite ? POLLOUT : POLLIN;
    const int r = poll(&p, 1, timeoutMs);
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p.revents & p.events)) {
        return -1;
    }
    return r;
#endif
}

#ifdef _WIN32
namespace {
struct WsaInit {
    WsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
WsaInit g_wsa;
}  // namespace
#endif

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(const std::string& host, uint16_t port, std::string* err) {
    close();

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);

    addrinfo hints{};
    // IPv4 only, same as the servers: a name that also has an IPv6 address
    // (localhost on most systems, for one) would otherwise be sent to over
    // v6 and never arrive, with nothing to say why.
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        if (err) *err = "cannot resolve '" + host + "'";
        return false;
    }

    long long f = (long long)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (f == XR_INVALID) {
        freeaddrinfo(res);
        if (err) *err = "socket() failed";
        return false;
    }

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)f, FIONBIO, &nb);
#else
    int flags = fcntl((int)f, F_GETFL, 0);
    fcntl((int)f, F_SETFL, flags | O_NONBLOCK);
#endif

    memcpy(addr_, res->ai_addr, res->ai_addrlen);
    addrLen_ = (int)res->ai_addrlen;
    freeaddrinfo(res);

    fd_ = (decltype(fd_))f;   // int on POSIX, long long on Windows
    endpoint_ = host + ":" + portStr;
    return true;
}

void UdpSocket::close() {
    if (fd_ != XR_INVALID) {
        XR_CLOSE(fd_);
        fd_ = XR_INVALID;
    }
    addrLen_ = 0;
    endpoint_.clear();
}

bool UdpSocket::send(const void* data, int len) {
    std::lock_guard<std::mutex> lk(sendMx_);
    if (fd_ == XR_INVALID || addrLen_ == 0) return false;
    int n = (int)sendto(
#ifdef _WIN32
        (SOCKET)fd_, (const char*)data, len, 0,
#else
        (int)fd_, data, (size_t)len, 0,
#endif
        (const sockaddr*)addr_, (socklen_t)addrLen_);
    return n == len;
}

int UdpSocket::recvWait(void* buf, int maxLen, int timeoutMs) {
    if (fd_ == XR_INVALID) return -1;
    const int r = waitSocket(fd_, false, timeoutMs);
    if (r <= 0) return r < 0 ? -1 : 0;        // error, or nothing within the timeout
    return recv(buf, maxLen);
}

int UdpSocket::recv(void* buf, int maxLen) {
    if (fd_ == XR_INVALID) return -1;
    sockaddr_storage from{};
    socklen_t fromLen = sizeof(from);
    int n = (int)recvfrom(
#ifdef _WIN32
        (SOCKET)fd_, (char*)buf, maxLen, 0,
#else
        (int)fd_, buf, (size_t)maxLen, 0,
#endif
        (sockaddr*)&from, &fromLen);
    if (n >= 0) return n;
    return XR_WOULDBLOCK ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Peer
// ---------------------------------------------------------------------------
std::string Peer::text() const {
    if (len <= 0) return "?";
    char host[NI_MAXHOST] = {0};
    char serv[NI_MAXSERV] = {0};
    if (getnameinfo((const sockaddr*)addr, (socklen_t)len, host, sizeof(host),
                    serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "?";
    }
    return std::string(host) + ":" + serv;
}

// ---------------------------------------------------------------------------
// UdpServerSocket
// ---------------------------------------------------------------------------
UdpServerSocket::~UdpServerSocket() { close(); }

bool UdpServerSocket::open(uint16_t port, const std::string& bindAddr, std::string* err) {
    close();

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", (unsigned)port);

    addrinfo hints{};
    // IPv4 only, deliberately: a dual-stack listener behaves differently on
    // Windows (v6only defaults on) than on Linux, and every client resolves
    // an IPv4 address anyway. One family means one behaviour everywhere.
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags    = AI_PASSIVE;

    addrinfo* res = nullptr;
    const char* node = bindAddr.empty() ? nullptr : bindAddr.c_str();
    if (getaddrinfo(node, portStr, &hints, &res) != 0 || res == nullptr) {
        if (err) *err = "cannot resolve bind address";
        return false;
    }

    long long f = (long long)socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (f == XR_INVALID) {
        freeaddrinfo(res);
        if (err) *err = "socket() failed";
        return false;
    }

    // UDP has no TIME_WAIT, so there is no reason to share the port -- and
    // every reason not to: on Windows SO_REUSEADDR lets a second listener
    // bind a port that is already taken, so "port in use" would never be
    // reported and datagrams would go to whichever socket won. Ask for the
    // opposite instead.
#ifdef _WIN32
    int exclusive = 1;
    setsockopt((SOCKET)f, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive,
               sizeof(exclusive));
#endif

    if (bind(
#ifdef _WIN32
            (SOCKET)f,
#else
            (int)f,
#endif
            res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
#ifdef _WIN32
        const int code = WSAGetLastError();
        const bool inUse = code == WSAEADDRINUSE;
        const bool denied = code == WSAEACCES;
#else
        const int code = errno;
        const bool inUse = code == EADDRINUSE;
        const bool denied = code == EACCES;
#endif
        freeaddrinfo(res);
        XR_CLOSE(f);
        if (err) {
            char msg[160];
            if (inUse) {
                snprintf(msg, sizeof(msg),
                         "port %u is already in use (another server running?)", (unsigned)port);
            } else if (denied) {
                snprintf(msg, sizeof(msg),
                         "not allowed to use port %u here -- pick one above 1024",
                         (unsigned)port);
            } else {
                snprintf(msg, sizeof(msg), "cannot listen on port %u (error %d)",
                         (unsigned)port, code);
            }
            *err = msg;
        }
        return false;
    }
    freeaddrinfo(res);

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)f, FIONBIO, &nb);
    // A client that has gone away makes Windows report the earlier sendto as
    // ECONNRESET on the *next* recvfrom, which would look like a dead socket.
    // Switching this off keeps one lost client from stopping the server.
#  ifdef SIO_UDP_CONNRESET
    DWORD off = 0;
    DWORD ret = 0;
    WSAIoctl((SOCKET)f, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret,
             nullptr, nullptr);
#  endif
#else
    int flags = fcntl((int)f, F_GETFL, 0);
    fcntl((int)f, F_SETFL, flags | O_NONBLOCK);
#endif

    fd_   = (decltype(fd_))f;
    port_ = port;
    return true;
}

void UdpServerSocket::close() {
    if (fd_ != XR_INVALID) {
        XR_CLOSE(fd_);
        fd_ = XR_INVALID;
    }
    port_ = 0;
}

int UdpServerSocket::recvFrom(void* buf, int maxLen, Peer* from, int timeoutMs) {
    if (fd_ == XR_INVALID) return -1;
    const int r = waitSocket(fd_, false, timeoutMs);
    if (r <= 0) return r < 0 ? -1 : 0;

    sockaddr_storage src{};
    socklen_t srcLen = sizeof(src);
    int n = (int)recvfrom(
#ifdef _WIN32
        (SOCKET)fd_, (char*)buf, maxLen, 0,
#else
        (int)fd_, buf, (size_t)maxLen, 0,
#endif
        (sockaddr*)&src, &srcLen);
    if (n < 0) return XR_WOULDBLOCK ? 0 : -1;

    if (from) {
        from->len = (int)srcLen > (int)sizeof(from->addr) ? (int)sizeof(from->addr)
                                                          : (int)srcLen;
        memcpy(from->addr, &src, (size_t)from->len);
    }
    return n;
}

bool UdpServerSocket::sendTo(const Peer& to, const void* data, int len) {
    std::lock_guard<std::mutex> lk(sendMx_);
    if (fd_ == XR_INVALID || to.len == 0) return false;
    int n = (int)sendto(
#ifdef _WIN32
        (SOCKET)fd_, (const char*)data, len, 0,
#else
        (int)fd_, data, (size_t)len, 0,
#endif
        (const sockaddr*)to.addr, (socklen_t)to.len);
    return n == len;
}

// Ask the routing table which local address would be used to reach the wider
// internet. Connecting a UDP socket sends nothing -- it only picks a route --
// so this works offline and costs nothing.
std::string localAddress() {
    long long f = (long long)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (f == XR_INVALID) return "";

    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port   = htons(53);
    probe.sin_addr.s_addr = inet_addr("1.1.1.1");

    std::string out;
    if (connect(
#ifdef _WIN32
            (SOCKET)f,
#else
            (int)f,
#endif
            (const sockaddr*)&probe, sizeof(probe)) == 0) {
        sockaddr_in me{};
        socklen_t meLen = sizeof(me);
        if (getsockname(
#ifdef _WIN32
                (SOCKET)f,
#else
                (int)f,
#endif
                (sockaddr*)&me, &meLen) == 0) {
            char host[NI_MAXHOST] = {0};
            if (getnameinfo((const sockaddr*)&me, meLen, host, sizeof(host),
                            nullptr, 0, NI_NUMERICHOST) == 0) {
                out = host;
            }
        }
    }
    XR_CLOSE(f);
    return out;
}

}  // namespace xr
