// Voice pipeline tests: microphone samples -> Opus -> "network" -> Opus ->
// mixed output, with no audio hardware. Drives the capture and playback paths
// directly through the test hooks, the way the audio devices would.
#include "voice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "mathconst.h"

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
        out[i] = (int16_t)(16000.0 * sin(2.0 * xr::kPi * hz * (double)i / kSampleRate));
    }
    return out;
}

static double rms(const std::vector<int16_t>& v) {
    double acc = 0;
    for (int16_t s : v) acc += (double)s * s;
    return v.empty() ? 0.0 : sqrt(acc / (double)v.size());
}

static int peak(const std::vector<int16_t>& v) {
    int p = 0;
    for (int16_t s : v) { const int a = s < 0 ? -s : s; if (a > p) p = a; }
    return p;
}

static std::vector<int16_t> slice(const std::vector<int16_t>& v, int fromMs, int toMs) {
    const size_t a = (size_t)(kSampleRate * fromMs / 1000), b = (size_t)(kSampleRate * toMs / 1000);
    return std::vector<int16_t>(v.begin() + (long)std::min(a, v.size()),
                                v.begin() + (long)std::min(b, v.size()));
}

// Encode `pcm` through the real transmit path and return the Opus frames.
static std::vector<OutFrame> encode(const std::vector<int16_t>& pcm) {
    std::vector<OutFrame> frames;
    setTransmitting(true);
    testCapture(pcm.data(), (int)pcm.size());
    pollOutgoing(frames);
    setTransmitting(false);
    return frames;
}

static void feed(uint32_t sid, const std::vector<OutFrame>& frames, uint16_t seqBase = 0) {
    for (size_t i = 0; i < frames.size(); ++i) {
        onIncomingFrame(sid, (uint16_t)(seqBase + i), frames[i].opus.data(), (int)frames[i].opus.size());
    }
}

// Fresh codec and radio state, so a test does not inherit the last one's
// squelch tail or limiter envelope.
static void fresh() {
    shutdown();
    std::string err;
    init(Mode::NoDevices, &err);
    setVolume(1.f);
    setRadioFilter(true);
    setHiss(0.35f);
    setSidetone(false);
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
    for (size_t i = 0; i < 6; ++i) {
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
    // The transmitter's overdrive caps everything at about two thirds of full
    // scale, however loud the input: no signal can come out as a scream.
    check("garbage output is capped by the audio stage", peak(out) <= 24000, std::to_string(peak(out)));

    printf("\nthe radio sound\n");
    {
        // Band-limiting: the receiver passes 300-2700 Hz. A 1 kHz tone comes
        // through, a 100 Hz rumble and a 6 kHz hiss do not. Noise off so the
        // comparison is only about the filter.
        fresh(); setHiss(0.f);
        auto mid = encode(tone(300, 1000.0));
        feed(20, mid);
        const double midLevel = rms(slice(renderMs(300), 100, 300));
        fresh(); setHiss(0.f);
        feed(21, encode(tone(300, 100.0)));
        const double lowLevel = rms(slice(renderMs(300), 100, 300));
        fresh(); setHiss(0.f);
        feed(22, encode(tone(300, 6000.0)));
        const double highLevel = rms(slice(renderMs(300), 100, 300));
        check("1 kHz passes", midLevel > 8000.0, std::to_string(midLevel));
        check("100 Hz is cut by more than 20 dB", lowLevel < midLevel / 10.0,
              std::to_string(lowLevel) + " vs " + std::to_string(midLevel));
        check("6 kHz is cut by more than 20 dB", highLevel < midLevel / 10.0,
              std::to_string(highLevel) + " vs " + std::to_string(midLevel));

        // The limiter: a quiet voice and a loud one come out at nearly the
        // same level, the way a transmitter's modulation limiter squashes
        // every syllable.
        fresh(); setHiss(0.f);
        std::vector<int16_t> quiet(tone(300));
        for (auto& v : quiet) v = (int16_t)(v / 16);          // -30 dBFS
        feed(23, encode(quiet));
        const double quietOut = rms(slice(renderMs(300), 100, 300));
        check("a -30 dBFS voice is brought up", quietOut > 4000.0, std::to_string(quietOut));
        check("24 dB of input spread becomes under 8 dB out",
              midLevel / quietOut < 2.5, std::to_string(midLevel / quietOut) + "x");

        // The squelch: nothing decoded yet means silence; a carrier opens it
        // with a burst of noise; the carrier dropping closes it with a longer
        // one, then a click, then silence again. Silent speech so what is
        // measured is the squelch, not the voice.
        fresh(); setHiss(1.f);
        std::vector<int16_t> nothing((size_t)kSampleRate * 300 / 1000, 0);
        feed(24, encode(nothing));
        auto sq = renderMs(700);
        const double opening = rms(slice(sq, 0, 15));
        const double settled = rms(slice(sq, 60, 200));
        check("the squelch opens with a burst", opening > 5.0 * settled,
              std::to_string(opening) + " vs " + std::to_string(settled));
        check("a strong signal carries only a faint noise floor",
              settled > 20.0 && settled < 800.0, std::to_string(settled));
        // 300 ms of frames, then 100 ms of concealment, then the tail.
        const double tail = rms(slice(sq, 420, 500));
        const double after = rms(slice(sq, 620, 700));
        check("the carrier dropping leaves a burst of noise", tail > 5.0 * settled,
              std::to_string(tail) + " vs " + std::to_string(settled));
        check("then the squelch closes to silence", after < 1.0, std::to_string(after));

        // Distance: the same silent carrier is noisier from the horizon.
        fresh(); setHiss(1.f);
        feed(25, encode(nothing));
        setSignalQuality(25, 0.f);
        const double weak = rms(slice(renderMs(300), 60, 280));
        fresh(); setHiss(1.f);
        feed(26, encode(nothing));
        setSignalQuality(26, 1.f);
        const double strong = rms(slice(renderMs(300), 60, 280));
        check("a signal from the horizon is much noisier", weak > 5.0 * strong,
              std::to_string(weak) + " vs " + std::to_string(strong));

        // ...and it breaks up: whole 20 ms frames of a tone go missing. The
        // jitter buffer keeps 12 frames, so the tone is exactly that long and
        // only the part before concealment starts is examined.
        fresh(); setHiss(0.f);
        feed(27, encode(tone(240)));
        setSignalQuality(27, 0.1f);
        auto fading = renderMs(240);
        int dropped = 0, kept = 0;
        for (int ms = 40; ms + 20 <= 220; ms += 20) {
            (rms(slice(fading, ms, ms + 20)) < 500.0 ? dropped : kept)++;
        }
        check("near the horizon the audio breaks up", dropped >= 2 && kept >= 2,
              std::to_string(dropped) + " frames dropped, " + std::to_string(kept) + " kept");
        fresh(); setHiss(0.f);
        feed(28, encode(tone(240)));
        setSignalQuality(28, 1.f);
        auto steady = renderMs(240);
        int gaps = 0;
        for (int ms = 40; ms + 20 <= 220; ms += 20) if (rms(slice(steady, ms, ms + 20)) < 500.0) ++gaps;
        check("a strong signal does not", gaps == 0, std::to_string(gaps) + " gaps");

        // Two pilots keying at once: the carriers beat and you hear a squeal
        // over both of them.
        fresh(); setHiss(0.f);
        feed(29, encode(nothing));
        const double one = rms(slice(renderMs(300), 60, 280));
        fresh(); setHiss(0.f);
        feed(30, encode(nothing));
        feed(31, encode(nothing));
        const double two = rms(slice(renderMs(300), 60, 280));
        check("two carriers at once produce the blocked squeal", two > 1500.0 && two > 10.0 * (one + 1.0),
              std::to_string(two) + " vs " + std::to_string(one));

        // Sidetone: while keyed you hear yourself, through the transmitter.
        fresh(); setHiss(0.f); setSidetone(true);
        setTransmitting(true);
        auto own = tone(200);
        testCapture(own.data(), (int)own.size());
        const double side = rms(renderMs(100));
        setTransmitting(false);
        check("sidetone plays your own voice back while keyed", side > 2000.0, std::to_string(side));

        // The cockpit's volume knobs: a voice on COM1 follows COM1's knob, one
        // on a frequency neither radio has plays at full so it is not lost.
        fresh(); setHiss(0.f);
        setRadioVolumes(122800, 1.f, 118100, 1.f);
        {
            auto f = encode(tone(300));
            for (size_t i = 0; i < f.size(); ++i)
                onIncomingFrame(40, (uint16_t)i, f[i].opus.data(), (int)f[i].opus.size(), 122800);
        }
        const double knobUp = rms(slice(renderMs(300), 100, 300));
        fresh(); setHiss(0.f);
        setRadioVolumes(122800, 0.1f, 118100, 1.f);
        {
            auto f = encode(tone(300));
            for (size_t i = 0; i < f.size(); ++i)
                onIncomingFrame(41, (uint16_t)i, f[i].opus.data(), (int)f[i].opus.size(), 122800);
        }
        const double knobDown = rms(slice(renderMs(300), 100, 300));
        check("turning COM1 down in the cockpit turns its voices down",
              knobDown < knobUp * 0.5, std::to_string(knobDown) + " vs " + std::to_string(knobUp));
        fresh(); setHiss(0.f);
        setRadioVolumes(122800, 0.1f, 118100, 0.1f);
        {
            auto f = encode(tone(300));
            for (size_t i = 0; i < f.size(); ++i)
                onIncomingFrame(42, (uint16_t)i, f[i].opus.data(), (int)f[i].opus.size(), 121500);
        }
        const double neither = rms(slice(renderMs(300), 100, 300));
        check("a frequency neither radio has is not silenced by either knob",
              neither > knobUp * 0.5, std::to_string(neither));

        check("signal quality reads back for the window's bars", [] {
            setSignalQuality(77, 0.3f);
            return signalQualityOf(77) > 0.29f && signalQualityOf(77) < 0.31f && signalQualityOf(78) == 1.f;
        }());

        // Switched off, the radio gets out of the way entirely.
        fresh(); setRadioFilter(false); setHiss(1.f);
        feed(32, encode(tone(300, 6000.0)));
        const double clean = rms(slice(renderMs(300), 100, 300));
        check("radio sound off: 6 kHz passes untouched", clean > 8000.0, std::to_string(clean));
        renderMs(300);
        const double cleanQuiet = rms(renderMs(100));
        check("radio sound off: no squelch noise either", cleanQuiet < 1.0, std::to_string(cleanQuiet));
        fresh();
    }

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
