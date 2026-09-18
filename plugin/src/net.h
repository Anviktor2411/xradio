// Minimal non-blocking UDP client socket, Windows + Linux + macOS.
#pragma once

#include <cstdint>
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

    const std::string& endpoint() const { return endpoint_; }

private:
#ifdef _WIN32
    // SOCKET is UINT_PTR on Windows; we keep it as long long and compare to -1.
    long long fd_ = -1;
#else
    int fd_ = -1;
#endif
    std::string endpoint_;
    // sockaddr_storage kept opaque so this header pulls in no platform headers.
    unsigned char addr_[128] = {0};
    int addrLen_ = 0;
};

}  // namespace xr
