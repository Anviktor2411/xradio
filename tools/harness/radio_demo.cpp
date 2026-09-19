// Renders a WAV through the real voice pipeline -- microphone path, Opus,
// the radio chain -- so the radio sound can be listened to without X-Plane.
//
//   ./radio_demo in.wav out_prefix
//
// Writes out_prefix_clean.wav (radio sound off), _strong.wav (next door),
// _far.wav (near the horizon), _blocked.wav (two people keying at once).
// Input: 16-bit mono PCM WAV at any rate (resampled to 48 kHz crudely).
#include "voice.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace xr::voice;

static std::vector<int16_t> readWav(const char* path, int* rate) {
    std::vector<int16_t> pcm;
    FILE* f = fopen(path, "rb");
    if (!f) return pcm;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return pcm;
    }
    int channels = 1, bits = 16;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t len; memcpy(&len, ch + 4, 4);
        if (!memcmp(ch, "fmt ", 4)) {
            std::vector<uint8_t> fmt(len);
            if (fread(fmt.data(), 1, len, f) != len) break;
            uint16_t nch; memcpy(&nch, fmt.data() + 2, 2); channels = nch;
            uint32_t sr;  memcpy(&sr, fmt.data() + 4, 4);  *rate = (int)sr;
            uint16_t bp;  memcpy(&bp, fmt.data() + 14, 2); bits = bp;
        } else if (!memcmp(ch, "data", 4)) {
            std::vector<uint8_t> data(len);
            if (fread(data.data(), 1, len, f) != len) break;
            if (bits != 16) break;
            const size_t n = len / 2;
            for (size_t i = 0; i < n; i += (size_t)channels) {
                int16_t v; memcpy(&v, data.data() + i * 2, 2);
                pcm.push_back(v);
            }
            break;
        } else {
            fseek(f, (long)len, SEEK_CUR);
        }
    }
    fclose(f);
    return pcm;
}

static void writeWav(const std::string& path, const std::vector<int16_t>& pcm, int rate) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t dataLen = (uint32_t)(pcm.size() * 2);
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + dataLen); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1); u32((uint32_t)rate);
    u32((uint32_t)rate * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(dataLen);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
}

// Linear resample to 48 kHz -- good enough for a demo.
static std::vector<int16_t> to48k(const std::vector<int16_t>& in, int rate) {
    if (rate == kSampleRate) return in;
    std::vector<int16_t> out((size_t)((double)in.size() * kSampleRate / rate));
    for (size_t i = 0; i < out.size(); ++i) {
        const double src = (double)i * rate / kSampleRate;
        const size_t a = (size_t)src;
        const double t = src - (double)a;
        const double v0 = in[a < in.size() ? a : in.size() - 1];
        const double v1 = in[a + 1 < in.size() ? a + 1 : in.size() - 1];
        out[i] = (int16_t)(v0 + (v1 - v0) * t);
    }
    return out;
}

// Push the whole clip through the transmit path and collect the frames.
static std::vector<OutFrame> encodeAll(const std::vector<int16_t>& pcm) {
    std::vector<OutFrame> all, part;
    setTransmitting(true);
    for (size_t off = 0; off < pcm.size(); off += 4800) {
        const int n = (int)std::min<size_t>(4800, pcm.size() - off);
        testCapture(pcm.data() + off, n);
        pollOutgoing(part);
        all.insert(all.end(), part.begin(), part.end());
    }
    setTransmitting(false);
    return all;
}

// Feed frames at their real pace and render: a second of lead-in silence,
// the transmission, then enough tail for the squelch to close.
static std::vector<int16_t> play(const std::vector<OutFrame>& frames,
                                 const std::vector<OutFrame>* second,
                                 float quality) {
    std::vector<int16_t> out;
    std::vector<int16_t> block(kFrameSamples);
    auto pull = [&] {
        testRender(block.data(), kFrameSamples);
        out.insert(out.end(), block.begin(), block.end());
    };
    for (int i = 0; i < 25; ++i) pull();                    // 0.5 s of nothing
    setSignalQuality(1, quality);
    setSignalQuality(2, quality);
    for (size_t i = 0; i < frames.size(); ++i) {
        onIncomingFrame(1, (uint16_t)i, frames[i].opus.data(), (int)frames[i].opus.size());
        if (second && i < second->size()) {
            onIncomingFrame(2, (uint16_t)i, (*second)[i].opus.data(), (int)(*second)[i].opus.size());
        }
        pull();
    }
    for (int i = 0; i < 50; ++i) pull();                    // tail + a second
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: radio_demo in.wav out_prefix\n"); return 2; }
    int rate = 0;
    auto raw = readWav(argv[1], &rate);        // sets rate; sequenced before use
    auto pcm = to48k(raw, rate);
    if (pcm.empty()) { fprintf(stderr, "cannot read %s (16-bit mono PCM WAV)\n", argv[1]); return 1; }
    const std::string prefix = argv[2];

    std::string err;
    if (!init(Mode::NoDevices, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    setVolume(1.f);
    setHiss(0.35f);
    auto frames = encodeAll(pcm);

    // A second voice for the blocked case: the same clip, pitched by
    // playing it 12% faster, so it is recognisably somebody else.
    std::vector<int16_t> other((size_t)((double)pcm.size() / 1.12));
    for (size_t i = 0; i < other.size(); ++i) {
        const size_t src = (size_t)((double)i * 1.12);
        other[i] = pcm[src < pcm.size() ? src : pcm.size() - 1];
    }
    auto frames2 = encodeAll(other);

    struct Case { const char* name; bool fx; float q; bool two; };
    const Case cases[] = {
        {"clean",   false, 1.f,  false},
        {"strong",  true,  1.f,  false},
        {"far",     true,  0.15f, false},
        {"blocked", true,  1.f,  true},
    };
    for (const Case& c : cases) {
        shutdown(); init(Mode::NoDevices, &err);
        setVolume(1.f); setHiss(0.35f); setRadioFilter(c.fx);
        auto out = play(frames, c.two ? &frames2 : nullptr, c.q);
        writeWav(prefix + "_" + c.name + ".wav", out, kSampleRate);
        printf("wrote %s_%s.wav  (%.1f s)\n", prefix.c_str(), c.name, (double)out.size() / kSampleRate);
    }
    shutdown();
    return 0;
}
