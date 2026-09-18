#include "net.h"

#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
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
#  include <sys/socket.h>
#  include <unistd.h>
#  define XR_INVALID (-1)
#  define XR_CLOSE(f) ::close((int)(f))
#  define XR_WOULDBLOCK (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

namespace xr {

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
    hints.ai_family   = AF_UNSPEC;      // IPv4 or IPv6, whichever resolves
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

    fd_ = f;
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

}  // namespace xr
