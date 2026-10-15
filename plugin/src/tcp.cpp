#include "tcp.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef int socklen_t;
#  define XR_TCP_CLOSE(f) closesocket((SOCKET)(f))
#  define XR_TCP_ERR WSAGetLastError()
#  define XR_TCP_INPROGRESS (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define XR_TCP_CLOSE(f) ::close((int)(f))
#  define XR_TCP_ERR errno
#  define XR_TCP_INPROGRESS (errno == EINPROGRESS)
#endif

namespace xr {

namespace {

#ifdef _WIN32
struct TcpWsaInit {
    TcpWsaInit()  { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~TcpWsaInit() { WSACleanup(); }
};
TcpWsaInit g_tcpWsa;
#endif

int waitFd(long long fd, bool forWrite, int timeoutMs) {
#ifdef _WIN32
    fd_set s; FD_ZERO(&s); FD_SET((SOCKET)fd, &s);
    fd_set e; FD_ZERO(&e); FD_SET((SOCKET)fd, &e);
    timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int r = select(0, forWrite ? nullptr : &s, forWrite ? &s : nullptr, &e, &tv);
    if (r > 0 && FD_ISSET((SOCKET)fd, &e)) return -1;
    return r;
#else
    pollfd p{}; p.fd = (int)fd; p.events = forWrite ? POLLOUT : POLLIN;
    const int r = poll(&p, 1, timeoutMs);
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p.revents & p.events)) return -1;
    return r;
#endif
}

void setBlocking(long long fd, bool blocking) {
#ifdef _WIN32
    u_long nb = blocking ? 0 : 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
#else
    const int fl = fcntl((int)fd, F_GETFL, 0);
    fcntl((int)fd, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

}  // namespace

TcpSocket::~TcpSocket() { close(); }

bool TcpSocket::connect(const std::string& host, uint16_t port, int timeoutMs, std::string* err) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portStr[16];
    snprintf(portStr, sizeof portStr, "%u", (unsigned)port);
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        if (err) *err = "cannot resolve " + host;
        return false;
    }
    std::string lastErr = "connect failed";
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        long long fd = (long long)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1) continue;
        setBlocking(fd, false);
        int r = ::connect((
#ifdef _WIN32
            SOCKET
#else
            int
#endif
            )fd, ai->ai_addr, (socklen_t)ai->ai_addrlen);
        bool ok = r == 0;
        if (!ok && XR_TCP_INPROGRESS) {
            if (waitFd(fd, true, timeoutMs) > 0) {
                int soerr = 0; socklen_t sl = sizeof soerr;
                getsockopt((
#ifdef _WIN32
                    SOCKET
#else
                    int
#endif
                    )fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl);
                ok = soerr == 0;
                if (!ok) lastErr = "connection refused";
            } else lastErr = "connection timed out";
        } else if (!ok) {
            lastErr = "connect error " + std::to_string((long long)XR_TCP_ERR);
        }
        if (ok) {
            setBlocking(fd, true);
            int one = 1;
            setsockopt((
#ifdef _WIN32
                SOCKET
#else
                int
#endif
                )fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            fd_ = fd;
            break;
        }
        XR_TCP_CLOSE(fd);
    }
    freeaddrinfo(res);
    if (fd_ == -1) { if (err) *err = lastErr; return false; }
    return true;
}

void TcpSocket::close() {
    std::lock_guard<std::mutex> lk(sendMx_);
    if (fd_ == -1) return;
#ifdef _WIN32
    shutdown((SOCKET)fd_, SD_BOTH);
#else
    shutdown((int)fd_, SHUT_RDWR);
#endif
    XR_TCP_CLOSE(fd_);
    fd_ = -1;
}

bool TcpSocket::send(const void* data, int len) {
    std::lock_guard<std::mutex> lk(sendMx_);
    if (fd_ == -1) return false;
    const char* p = (const char*)data;
    while (len > 0) {
        const int n = (int)::send((
#ifdef _WIN32
            SOCKET
#else
            int
#endif
            )fd_, p, (size_t)len,
#ifdef _WIN32
            0
#else
            MSG_NOSIGNAL
#endif
        );
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

int TcpSocket::recv(void* buf, int maxLen, int timeoutMs) {
    const long long fd = fd_;
    if (fd == -1) return -1;
    const int w = waitFd(fd, false, timeoutMs);
    if (w == 0) return 0;
    if (w < 0) return -1;
    const int n = (int)::recv((
#ifdef _WIN32
        SOCKET
#else
        int
#endif
        )fd, (char*)buf, (size_t)maxLen, 0);
    return n <= 0 ? -1 : n;
}

}  // namespace xr
