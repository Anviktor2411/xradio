// Minimal UDP client socket, Windows + Linux + macOS.
//
// send() may be called from any thread; open()/close() and recv*() are meant
// for one thread at a time (the plugin stops its network thread around a
// reconnect).
#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace xr {

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    // Resolves `host` and opens a non-blocking UDP socket aimed at host:port.
    bool open(const std::string& host, uint16_t port, std::string* err);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    bool send(const void* data, int len);

    // Returns the number of bytes read, 0 if nothing is waiting, -1 on error.
    int recv(void* buf, int maxLen);

    // Like recv(), but waits up to `timeoutMs` for something to arrive.
    int recvWait(void* buf, int maxLen, int timeoutMs);

    const std::string& endpoint() const { return endpoint_; }

private:
#ifdef _WIN32
    // SOCKET is UINT_PTR on Windows; we keep it as long long and compare to -1.
    long long fd_ = -1;
#else
    int fd_ = -1;
#endif
    std::mutex  sendMx_;
    std::string endpoint_;
    // sockaddr_storage kept opaque so this header pulls in no platform headers.
    unsigned char addr_[128] = {0};
    int addrLen_ = 0;
};

// Who a datagram came from. Opaque bytes so this header stays free of
// platform sockets; compare and copy it, do not look inside.
struct Peer {
    unsigned char addr[128] = {0};
    int           len = 0;

    bool operator==(const Peer& o) const {
        return len == o.len && memcmp(addr, o.addr, (size_t)len) == 0;
    }
    bool operator<(const Peer& o) const {      // so it can key a std::map
        if (len != o.len) return len < o.len;
        return memcmp(addr, o.addr, (size_t)len) < 0;
    }
    std::string text() const;                  // "1.2.3.4:49100", for logs
};

// A bound UDP socket: receives from anyone, replies to a specific peer.
// This is what the built-in server listens on, so a pilot can host a
// flight without installing anything.
class UdpServerSocket {
public:
    UdpServerSocket() = default;
    ~UdpServerSocket();
    UdpServerSocket(const UdpServerSocket&) = delete;
    UdpServerSocket& operator=(const UdpServerSocket&) = delete;

    // Binds `port` on every interface. `bindAddr` empty means 0.0.0.0.
    bool open(uint16_t port, const std::string& bindAddr, std::string* err);
    void close();
    bool isOpen() const { return fd_ >= 0; }

    // Waits up to timeoutMs. Returns bytes read, 0 on timeout, -1 on error.
    int recvFrom(void* buf, int maxLen, Peer* from, int timeoutMs);
    bool sendTo(const Peer& to, const void* data, int len);

    uint16_t boundPort() const { return port_; }

private:
#ifdef _WIN32
    long long fd_ = -1;
#else
    int fd_ = -1;
#endif
    uint16_t   port_ = 0;
    std::mutex sendMx_;
};

// Wait up to timeoutMs for a socket to be readable (or writable). >0 ready,
// 0 timed out, <0 error. poll() on POSIX, so descriptors above FD_SETSIZE
// are fine; select() on Windows, where they are.
int waitSocket(long long fd, bool forWrite, int timeoutMs);

// Best guess at this machine's address on the local network, for the
// "tell your friends to type this" line. Empty if it cannot be worked out.
std::string localAddress();

}  // namespace xr
