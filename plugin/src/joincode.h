// Join codes: an address and port as ten letters, e.g. "K7M2Q-X4PB9".
//
// Reading "81.90.144.12 port 49100" over voice chat or a phone is error
// prone; ten symbols from an alphabet with no 0/O or 1/I/L confusion is not.
// The code is nothing but the IPv4 address and port packed into 48 bits plus
// a 2-bit check, in Crockford base32 -- no server is involved, so it works
// for a self-hosted flight with nothing else running anywhere.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace xr {
namespace joincode {

// Crockford's alphabet: digits then letters, skipping I, L, O, U.
inline const char* alphabet() { return "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; }

inline int symbolValue(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == 'O') c = '0';                 // the usual misreadings
    if (c == 'I' || c == 'L') c = '1';
    const char* a = alphabet();
    for (int i = 0; a[i]; ++i) if (a[i] == c) return i;
    return -1;
}

// Dotted-quad to 32 bits. False for anything that is not one.
inline bool parseIPv4(const std::string& s, uint32_t* out) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

inline std::string formatIPv4(uint32_t ip) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255,
             (ip >> 8) & 255, ip & 255);
    return buf;
}

inline uint64_t checkBits(uint64_t v) {
    uint64_t sum = 0;
    for (int i = 0; i < 6; ++i) sum += (v >> (8 * i)) & 255;
    return sum & 3;
}

// "" if the address is not an IPv4 dotted quad.
inline std::string encode(const std::string& ipv4, uint16_t port) {
    uint32_t ip = 0;
    if (!parseIPv4(ipv4, &ip) || port == 0) return "";
    const uint64_t v = ((uint64_t)ip << 16) | port;           // 48 bits
    const uint64_t full = (v << 2) | checkBits(v);           // 50 bits, check last
    std::string out;
    for (int i = 9; i >= 0; --i) out += alphabet()[(full >> (5 * i)) & 31];
    out.insert(5, "-");
    return out;
}

// Accepts "K7M2Q-X4PB9", "k7m2q x4pb9", "K7M2QX4PB9". False if it is not a
// code or its check bits do not match.
inline bool decode(const std::string& code, std::string* ipv4, uint16_t* port) {
    std::string sym;
    for (char c : code) {
        if (c == '-' || c == ' ') continue;
        sym += c;
    }
    if (sym.size() != 10) return false;
    uint64_t full = 0;
    for (char c : sym) {
        const int v = symbolValue(c);
        if (v < 0) return false;
        full = (full << 5) | (uint64_t)v;
    }
    const uint64_t v = full >> 2;
    if ((full & 3) != checkBits(v)) return false;
    if (port) *port = (uint16_t)(v & 0xFFFF);
    if (ipv4) *ipv4 = formatIPv4((uint32_t)(v >> 16));
    return (v & 0xFFFF) != 0;
}

// Does this look like a join code rather than a host name? Codes have the
// dash in the middle and nothing a host name would have.
inline bool looksLikeCode(const std::string& s) {
    std::string t;
    for (char c : s) if (c != ' ') t += c;
    if (t.size() != 11 || t[5] != '-') return false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i == 5) continue;
        if (symbolValue(t[i]) < 0) return false;
    }
    return true;
}

}  // namespace joincode
}  // namespace xr
