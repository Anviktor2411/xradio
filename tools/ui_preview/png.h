// A minimal PNG writer (stored, uncompressed deflate blocks) -- enough for screenshots, no library needed.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

inline uint32_t pngCrc(const unsigned char* d, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
inline void pngChunk(FILE* f, const char* type, const std::vector<unsigned char>& data) {
    unsigned char len[4] = {(unsigned char)(data.size() >> 24), (unsigned char)(data.size() >> 16), (unsigned char)(data.size() >> 8), (unsigned char)data.size()};
    fwrite(len, 1, 4, f);
    std::vector<unsigned char> td(type, type + 4); td.insert(td.end(), data.begin(), data.end());
    fwrite(td.data(), 1, td.size(), f);
    const uint32_t crc = pngCrc(td.data(), td.size()) ^ 0xFFFFFFFFu;
    unsigned char c[4] = {(unsigned char)(crc >> 24), (unsigned char)(crc >> 16), (unsigned char)(crc >> 8), (unsigned char)crc};
    fwrite(c, 1, 4, f);
}
inline bool writePng(const std::string& path, int w, int h, const unsigned char* rgba) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<unsigned char> ihdr = {(unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w, (unsigned char)(h >> 24), (unsigned char)(h >> 16), (unsigned char)(h >> 8), (unsigned char)h, 8, 6, 0, 0, 0};
    pngChunk(f, "IHDR", ihdr);
    // raw scanlines with filter byte 0
    std::vector<unsigned char> raw;
    raw.reserve((size_t)h * (w * 4 + 1));
    for (int y = 0; y < h; ++y) { raw.push_back(0); raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4); }
    // zlib stream of stored blocks
    std::vector<unsigned char> z = {0x78, 0x01};
    size_t pos = 0;
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    while (pos < raw.size()) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((unsigned char)(n & 0xFF)); z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)(~n & 0xFF)); z.push_back((unsigned char)((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    const uint32_t adler = (b << 16) | a;
    z.push_back((unsigned char)(adler >> 24)); z.push_back((unsigned char)(adler >> 16)); z.push_back((unsigned char)(adler >> 8)); z.push_back((unsigned char)adler);
    pngChunk(f, "IDAT", z);
    pngChunk(f, "IEND", {});
    fclose(f);
    return true;
}
