// A blocking-with-timeout TCP client socket for the TowerTrainer link, Windows + Linux + macOS.
// Kept apart from net.h (UDP) so that file stays what it says it is. One thread owns a TcpSocket;
// send() and close() may be called from another thread (close() is how that thread is woken up).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace xr {

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Resolves host and connects within timeoutMs. False with the reason in *err.
    bool connect(const std::string& host, uint16_t port, int timeoutMs, std::string* err);
    void close();
    bool isOpen() const { return fd_ != -1; }

    // Sends everything or returns false.
    bool send(const void* data, int len);
    bool sendLine(const std::string& line) { const std::string l = line + "\n"; return send(l.data(), (int)l.size()); }

    // Bytes read (>0), 0 on timeout, -1 when closed or failed.
    int recv(void* buf, int maxLen, int timeoutMs);

private:
    long long   fd_ = -1;
    std::mutex  sendMx_;
};

}  // namespace xr
