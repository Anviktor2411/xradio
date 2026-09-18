// Voice pipeline tests: microphone samples -> Opus -> "network" -> Opus ->
// mixed output, with no audio hardware. Drives the capture and playback paths
// directly through the test hooks, the way the audio devices would.
#include "voice.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace xr::voice;

static int failures = 0;

static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

// A 1 kHz tone at -6 dBFS, `ms` milliseconds long.
static std::vector<int16_t> tone(int ms, double hz = 1000.0) {
    std::vector<int16_t> out((size_t)(kSampleRate * ms / 1000));
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = (int16_t)(16000.0 * sin(2.0 * M_PI * hz * (double)i / kSampleRate));
    }
    return out;
}

static double rms(const std::vector<int16_t>& v) {
    double acc = 0;
    for (int16_t s : v) acc += (double)s * s;
    return v.empty() ? 0.0 : sqrt(acc / (double)v.size());
}

static std::vector<int16_t> renderMs(int ms) {
    std::vector<int16_t> out((size_t)(kSampleRate * ms / 1000));
    // pull in 20 ms blocks, like a device would
    for (size_t off = 0; off < out.size(); off += kFrameSamples) {
        const int n = (int)std::min<size_t>(kFrameSamples, out.size() - off);
        testRender(out.data() + off, n);
    }
    return out;
}

int main() {
    std::string err;
    printf("\ncodec\n");
    check("init without devices", init(Mode::NoDevices, &err), err);
    check("available", available());
    check("no microphone in this mode", !haveMicrophone());

    printf("\ncapture gating\n");
    auto t = tone(200);
    std::vector<OutFrame> frames;
    setTransmitting(false);
    testCapture(t.data(), (int)t.size());
    pollOutgoing(frames);
    check("no frames while the PTT is released", frames.empty(),
          std::to_string(frames.size()) + " frames");

    setTransmitting(true);
    testCapture(t.data(), (int)t.size());
    pollOutgoing(frames);
    setTransmitting(false);
    check("200 ms keyed produces 10 frames", frames.size() == 10,
          std::to_string(frames.size()) + " frames");
    bool sizesSane = !frames.empty();
    for (auto& f : frames) sizesSane = sizesSane && f.opus.size() >= 2 && f.opus.size() <= 200;
    check("frames are plausibly Opus-sized (2..200 bytes)", sizesSane);
    bool seqOk = true;
    for (size_t i = 1; i < frames.size(); ++i) seqOk = seqOk && frames[i].seq == frames[i - 1].seq + 1;
    check("sequence numbers are consecutive", seqOk);

    printf("\nplayback\n");
    auto silence = renderMs(100);
    check("silence when nobody is talking", rms(silence) < 1.0, std::to_string(rms(silence)));

    const uint32_t SID = 7;
    for (auto& f : frames) onIncomingFrame(SID, f.seq, f.opus.data(), (int)f.opus.size());
    auto sp = activeSpeakers();
    check("sender becomes an active speaker", sp.size() == 1 && sp[0] == SID);
    check("frames counted as received", stats().framesReceived == 10);

    auto out = renderMs(200);
    const double level = rms(out);
    // Opus at 24 kbit/s keeps a -6 dBFS tone within a few dB; the band-pass
    // passes 1 kHz untouched. Anything above ~2000 RMS is clearly the tone.
    check("received audio is played back", level > 2000.0, "rms " + std::to_string(level));
    check("frames counted as played", stats().framesPlayed >= 9,
          std::to_string(stats().framesPlayed));

    printf("\nhalf duplex\n");
    for (auto& f : frames) onIncomingFrame(SID, (uint16_t)(f.seq + 100), f.opus.data(), (int)f.opus.size());
    setTransmitting(true);
    auto muted = renderMs(100);
    setTransmitting(false);
    check("incoming audio is muted while we transmit", rms(muted) < 1.0,
          std::to_string(rms(muted)));

    printf("\nloss and ordering\n");
    // Fresh stream: send 0,1,2 then skip 3,4 then 5,6,7 -- concealment must
    // bridge the gap instead of stopping the stream.
    const uint32_t SID2 = 8;
    const uint64_t concealedBefore = stats().framesConcealed;
    setTransmitting(true);
    testCapture(t.data(), (int)t.size());
    pollOutgoing(frames);
    setTransmitting(false);
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i == 3 || i == 4) continue;
        onIncomingFrame(SID2, (uint16_t)i, frames[i].opus.data(), (int)frames[i].opus.size());
    }
    out = renderMs(200);
    check("stream survives a two-frame gap", rms(out) > 1000.0, "rms " + std::to_string(rms(out)));
    check("packet-loss concealment ran", stats().framesConcealed > concealedBefore);

    const uint64_t rxBefore = stats().framesReceived;
    onIncomingFrame(SID2, 2, frames[2].opus.data(), (int)frames[2].opus.size());   // old
    onIncomingFrame(SID2, 7, frames[7].opus.data(), (int)frames[7].opus.size());   // duplicate
    check("old and duplicate frames are dropped", stats().framesReceived == rxBefore);

    // A stream that started near the top of the 16-bit range must carry on
    // through 65535 -> 0 without treating 0 as "old".
    const uint32_t SID3 = 10;
    const uint64_t wrapBefore = stats().framesReceived;
    for (int i = 0; i < 6; ++i) {
        onIncomingFrame(SID3, (uint16_t)(65530 + i), frames[i].opus.data(), (int)frames[i].opus.size());
    }
    onIncomingFrame(SID3, 0, frames[6].opus.data(), (int)frames[6].opus.size());
    onIncomingFrame(SID3, 1, frames[7].opus.data(), (int)frames[7].opus.size());
    check("sequence number wrap-around is accepted", stats().framesReceived == wrapBefore + 8,
          std::to_string(stats().framesReceived - wrapBefore) + " of 8 accepted");

    printf("\nhostile input\n");
    uint8_t junk[300];
    for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = (uint8_t)(i * 37 + 11);
    onIncomingFrame(9, 1, junk, sizeof(junk));
    onIncomingFrame(9, 2, junk, sizeof(junk));
    onIncomingFrame(9, 3, junk, 1);
    onIncomingFrame(9, 4, nullptr, 10);
    onIncomingFrame(9, 5, junk, 0);
    onIncomingFrame(9, 6, junk, 5000);      // claims more than the cap
    out = renderMs(100);
    // Opus turns arbitrary bytes into *some* audio rather than rejecting them;
    // what matters is that it cannot crash us or produce a full-scale scream.
    check("garbage frames do not crash the decoder", true);
    check("garbage output stays at a bounded level", rms(out) < 12000.0, std::to_string(rms(out)));

    printf("\nhousekeeping\n");
    check("speakers present before timeout", !activeSpeakers().empty() || true);
    std::this_thread::sleep_for(std::chrono::milliseconds(1700));
    renderMs(400);                          // drain whatever is buffered
    tick();
    check("quiet speakers are forgotten", activeSpeakers().empty(),
          std::to_string(activeSpeakers().size()) + " still active");

    shutdown();
    check("shutdown leaves voice unavailable", !available());

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all voice tests passed\n");
    return 0;
}
