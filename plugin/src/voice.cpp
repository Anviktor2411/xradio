#include "voice.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include "mathconst.h"

#ifndef XRADIO_USE_VOICE
// ---------------------------------------------------------------------------
// Stub build: no Opus / miniaudio available.
// ---------------------------------------------------------------------------
namespace xr {
namespace voice {
bool init(Mode, std::string* err) {
    if (err) *err = "built without voice (configure with -DXRADIO_USE_VOICE=ON)";
    return false;
}
bool init(Mode m, const std::string&, const std::string&, std::string* err) { return init(m, err); }
std::vector<Device> listDevices(bool) { return {}; }
bool reopenDevices(const std::string&, const std::string&, std::string* err) {
    if (err) *err = "built without voice";
    return false;
}
static const std::string g_none;
const std::string& currentMic() { return g_none; }
const std::string& currentOutput() { return g_none; }
void setSidetone(bool) {}
void setHiss(float) {}
void setRadioFilter(bool) {}
void setSignalQuality(uint32_t, float) {}
void shutdown() {}
bool available() { return false; }
bool haveMicrophone() { return false; }
void setTransmitting(bool) {}
bool transmitting() { return false; }
void onIncomingFrame(uint32_t, uint16_t, const uint8_t*, int, uint32_t) {}
void setRadioVolumes(uint32_t, float, uint32_t, float) {}
float signalQualityOf(uint32_t) { return 1.f; }
void pollOutgoing(std::vector<OutFrame>& out) { out.clear(); }
void tick() {}
void setVolume(float) {}
float micLevel() { return 0.f; }
std::vector<uint32_t> activeSpeakers() { return {}; }
std::string status() { return "not built in"; }
Stats stats() { return {}; }
void testCapture(const int16_t*, int) {}
void testRender(int16_t* out, int count) { if (out) memset(out, 0, sizeof(int16_t) * (size_t)count); }
}  // namespace voice
}  // namespace xr

#else
// ---------------------------------------------------------------------------
// Real build.
// ---------------------------------------------------------------------------

// miniaudio is a single header; this translation unit holds its implementation.
// We only need device I/O -- switch off the parts that pull in extra code.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wshadow"
#elif defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "miniaudio.h"
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif

#include <opus.h>

namespace xr {
namespace voice {

namespace {

constexpr int   kBitrate      = 24000;
constexpr int   kMaxOpusBytes = 512;   // 24 kbit/s * 20 ms is ~60 bytes; plenty of headroom
constexpr int   kPrebuffer    = 2;     // frames buffered before a speaker starts playing
constexpr int   kMaxJitter    = 12;    // frames; older ones are dropped beyond this
constexpr int   kMaxConceal   = 5;     // lost frames concealed before a speaker goes quiet
constexpr int   kMaxOutQueue  = 25;    // half a second of encoded audio, if nobody sends it
constexpr float kSpeakerTimeoutS = 1.5f;
constexpr int   kMaxSpeakers  = 16;    // each one owns a decoder; a hostile server must not
                                       // be able to allocate thousands

// One remote pilot's stream. The decoder lives here and is only ever touched
// from the playback side, so concealment can run without a second lock.
struct Speaker {
    OpusDecoder*                     dec = nullptr;
    std::deque<std::vector<uint8_t>> packets;    // encoded, oldest first
    std::vector<int16_t>             pcm;        // decoded samples still to play
    size_t                           pcmPos = 0;
    uint16_t                         lastSeq = 0;
    bool                             haveSeq = false;
    bool                             playing = false;
    bool                             dropped = false;   // this frame lost to fading
    uint32_t                         freqKhz = 0;       // what radio it is coming in on
    float                            txEnv = 0.f;       // their transmitter's limiter
    int                              concealed = 0;
    std::chrono::steady_clock::time_point lastRx;

    ~Speaker() { if (dec) opus_decoder_destroy(dec); }
};

// Guards g_speakers and g_out. Held for microseconds at a time.
std::mutex                                     g_mx;
std::map<uint32_t, std::unique_ptr<Speaker>>   g_speakers;
std::deque<OutFrame>                           g_out;
uint16_t                                       g_seq = 0;

OpusEncoder*      g_enc = nullptr;
ma_context        g_ctx;
ma_device         g_cap;
ma_device         g_play;
bool              g_ctxOk = false, g_capOk = false, g_playOk = false;
Mode              g_mode = Mode::NoDevices;

std::atomic<bool>  g_tx{false};
std::atomic<bool>  g_sidetone{false};
std::atomic<bool>  g_filter{true};
std::atomic<float> g_hiss{0.35f};
std::atomic<float> g_micLevel{0.f};
std::atomic<float> g_volume{1.f};
// the audio panel: tuned frequencies and knob positions, main thread -> playback
std::atomic<uint32_t> g_com1Khz{0}, g_com2Khz{0};
std::atomic<float>    g_com1Vol{1.f}, g_com2Vol{1.f};

// How far up the knob is for the radio this frequency is on. A frequency
// neither radio has (or 0) plays at full: better heard than silently lost.
float radioGain(uint32_t freqKhz) {
    if (freqKhz == 0) return 1.f;
    if (freqKhz == g_com1Khz.load()) return g_com1Vol.load();
    if (freqKhz == g_com2Khz.load()) return g_com2Vol.load();
    return 1.f;
}
std::atomic<uint64_t> g_nEncoded{0}, g_nReceived{0}, g_nPlayed{0}, g_nConcealed{0};

std::vector<int16_t> g_capAccum;     // capture thread only
std::string          g_status = "off";
std::string          g_capName, g_playName;
std::string          g_wantMic, g_wantOut;      // names the user picked, "" = default

// Device lists, refreshed on init/reopen. The ma_device_id is what actually
// selects a device; the name is only how the user recognises it.
struct DevEntry { std::string name; ma_device_id id; bool isDefault; };
std::vector<DevEntry> g_capDevs, g_playDevs;

// Sidetone: a copy of what the microphone just captured, queued for playback
// so the speaker hears themselves. Guarded by g_mx like everything else.
std::deque<int16_t> g_sidechain;
constexpr size_t kSidechainMax = 48000 / 2;      // half a second

// --- the sound of a VHF radio ----------------------------------------------
// A band-pass on its own sounds like a telephone. What makes a COM radio
// sound like one is everything else in the chain, so all of it is modelled,
// in the order the real signal goes through it:
//
//   transmitter  the modulation limiter squashes every syllable to the same
//                level, and over-modulation gives consonants their crunch
//   channel      noise that rises as the other aircraft nears the horizon,
//                the audio breaking up out there, and the heterodyne squeal
//                when two people key at once
//   receiver     the squelch opening with a click, closing with a burst of
//                noise, and the narrow 300-2700 Hz audio filter
//
// All of it runs on the playback thread, per sample, with no allocation.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    float run(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.f; }
};

void designHighpass(Biquad& q, float fc, float Q) {
    const float w = 2.f * (float)kPi * fc / (float)kSampleRate;
    const float c = cosf(w), s = sinf(w), alpha = s / (2.f * Q);
    const float a0 = 1.f + alpha;
    q.b0 = (1.f + c) / 2.f / a0; q.b1 = -(1.f + c) / a0; q.b2 = (1.f + c) / 2.f / a0;
    q.a1 = -2.f * c / a0;        q.a2 = (1.f - alpha) / a0;
}

void designLowpass(Biquad& q, float fc, float Q) {
    const float w = 2.f * (float)kPi * fc / (float)kSampleRate;
    const float c = cosf(w), s = sinf(w), alpha = s / (2.f * Q);
    const float a0 = 1.f + alpha;
    q.b0 = (1.f - c) / 2.f / a0; q.b1 = (1.f - c) / a0; q.b2 = (1.f - c) / 2.f / a0;
    q.a1 = -2.f * c / a0;        q.a2 = (1.f - alpha) / a0;
}

// Levels are in int16 units. The hiss setting (0..1) scales the noise ones.
constexpr float kLimThreshold = 2000.f;   // -24 dBFS: everything above comes out the same
constexpr float kLimMakeup    = 9.f;
constexpr float kDrive        = 1.3f;     // into the overdrive
constexpr float kClipLevel    = 20000.f;  // the overdrive's ceiling
constexpr float kAudioMax     = 24000.f;  // the receiver's audio stage: nothing louder

// Cubic soft clipper: unity gain for small signals, a gentle bend towards
// `level`, and never above it. The bend is what adds the harmonics that
// make an over-modulated voice sound the way it does.
inline float softClip(float x, float level) {
    const float full = level * 1.5f;
    float s = x / full;
    if (s > 1.f) s = 1.f; else if (s < -1.f) s = -1.f;
    return (s - s * s * s / 3.f) * full;
}
// The noise is white before the 300-2700 Hz filter, which keeps about a
// tenth of it, so what comes out is ~15 dB below these figures. At the
// default noise setting (0.35) that puts a strong signal 50 dB under the
// speech, one at the horizon about 15 dB under it -- readable, noisy -- and
// at the full setting 6 dB, which is a proper weak-signal struggle.
constexpr float kNoiseStrong  = 250.f;
constexpr float kNoiseWeak    = 13700.f;
constexpr float kNoiseBurst   = 16000.f;  // squelch opening and closing
constexpr float kHetAmp       = 4000.f;   // two carriers on one frequency
constexpr float kClickAmp     = 14000.f;
constexpr int   kClickLen     = 96;       // 2 ms step, the filter turns it into a click
constexpr int   kOpenBurst    = 960;      // 20 ms of noise as the squelch opens
constexpr int   kCloseTail    = 5280;     // 110 ms of noise after the carrier drops
constexpr int   kSettle       = 1440;     // 30 ms: let the filters ring down

struct Radio {
    Biquad hp1, hp2, lp1, lp2;            // 4th order each side
    float  env = 0.f;                     // our own transmitter's envelope (sidetone)
    bool   open = false;                  // squelch state
    int    burst = 0, tail = 0, click = 0, settle = 0;
    float  clickSign = 1.f;
    float  hetPhase = 0.f, wobble = 0.f;
    uint32_t noise = 0x12345678u;

    void design() {
        // Butterworth pair for a 4th-order slope: 24 dB/octave.
        designHighpass(hp1, 300.f, 0.5412f);
        designHighpass(hp2, 300.f, 1.3066f);
        designLowpass(lp1, 2700.f, 0.5412f);
        designLowpass(lp2, 2700.f, 1.3066f);
    }

    float white() {
        noise = noise * 1664525u + 1013904223u;
        return (float)(noise >> 16) / 32768.f - 1.f;
    }

    // The transmitter: modulation limiter, then overdrive. Speech only. Each
    // pilot's radio has its own limiter, so the envelope is theirs, not ours.
    static float transmitter(float& env, float x) {
        const float a = x < 0.f ? -x : x;
        // ~1 ms attack, ~120 ms release
        env += (a > env ? 0.02f : 0.00017f) * (a - env);
        const float g = env > kLimThreshold ? kLimThreshold / env : 1.f;
        return softClip(x * g * kLimMakeup * kDrive, kClipLevel);
    }

    float filter(float x) { return lp2.run(lp1.run(hp2.run(hp1.run(x)))); }

    // The audio amplifier at the end of the chain. Speech is already held
    // down by the overdrive, but click + noise + squeal on top of it could
    // still add up past full scale; this keeps the sum inside kAudioMax.
    static float amplifier(float x) { return softClip(x, kAudioMax); }

    // Called once per callback with what the mixer found.
    void gate(bool carrier) {
        if (carrier && !open)  { open = true;  burst = kOpenBurst; click = kClickLen; clickSign = 1.f; }
        if (!carrier && open)  { open = false; tail = kCloseTail; }
        if (carrier || tail || burst || click) settle = kSettle;
    }
    bool active() const { return open || tail > 0 || burst > 0 || click > 0 || settle > 0; }

    // One output sample. `speech` is the mixed, already-transmitter-processed
    // voice (0 when nobody is talking), `quality` the best signal among the
    // carriers (1 next door, 0 at the horizon), `carriers` how many.
    float receiver(float speech, bool carrier, int carriers, float quality, float hiss) {
        float noiseAmp = 0.f;
        if (tail > 0) {
            noiseAmp = hiss * kNoiseBurst;
            if (--tail == 0) { click = kClickLen; clickSign = -1.f; }
        } else if (carrier) {
            const float w = 1.f - quality;
            noiseAmp = hiss * (kNoiseStrong + (kNoiseWeak - kNoiseStrong) * w);
            if (burst > 0) { noiseAmp = noiseAmp > hiss * kNoiseBurst ? noiseAmp : hiss * kNoiseBurst; --burst; }
        }
        float x = carrier ? speech : 0.f;
        if (noiseAmp > 0.f) x += white() * noiseAmp;

        if (carrier && carriers >= 2) {
            // Two AM carriers a little apart beat against each other.
            wobble += 2.f * (float)kPi * 0.6f / (float)kSampleRate;
            if (wobble > 2.f * (float)kPi) wobble -= 2.f * (float)kPi;
            const float f = 1300.f + 250.f * sinf(wobble);
            hetPhase += 2.f * (float)kPi * f / (float)kSampleRate;
            if (hetPhase > 2.f * (float)kPi) hetPhase -= 2.f * (float)kPi;
            x += kHetAmp * sinf(hetPhase);
        }
        if (click > 0) {
            x += clickSign * kClickAmp * (float)click / (float)kClickLen;
            --click;
        }
        if (settle > 0 && !carrier && tail == 0 && burst == 0 && click == 0) --settle;
        return amplifier(filter(x));
    }
};

Radio g_radio;

void captureCb(ma_device*, void*, const void* in, ma_uint32 n);
void playbackCb(ma_device*, void* out, const void*, ma_uint32 n);

// --- capture side ----------------------------------------------------------
void capture(const int16_t* in, int count) {
    int peak = 0;
    for (int i = 0; i < count; ++i) {
        const int a = in[i] < 0 ? -in[i] : in[i];
        if (a > peak) peak = a;
    }
    g_micLevel.store((float)peak / 32767.f);

    if (!g_tx.load() || !g_enc) {
        g_capAccum.clear();
        return;
    }

    if (g_sidetone.load()) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_sidechain.insert(g_sidechain.end(), in, in + count);
        while (g_sidechain.size() > kSidechainMax) g_sidechain.pop_front();
    }

    g_capAccum.insert(g_capAccum.end(), in, in + count);
    while (g_capAccum.size() >= (size_t)kFrameSamples) {
        uint8_t buf[kMaxOpusBytes];
        const int len = opus_encode(g_enc, g_capAccum.data(), kFrameSamples, buf, sizeof(buf));
        g_capAccum.erase(g_capAccum.begin(), g_capAccum.begin() + kFrameSamples);
        if (len <= 0) continue;

        std::lock_guard<std::mutex> lk(g_mx);
        g_out.push_back({g_seq++, std::vector<uint8_t>(buf, buf + len)});
        if (g_out.size() > (size_t)kMaxOutQueue) g_out.pop_front();
        g_nEncoded.fetch_add(1);
    }
}

// --- playback side ---------------------------------------------------------
// Fills sp.pcm with the next 20 ms: the next packet if there is one, else
// concealment. Returns false once the speaker has run dry for good.
bool refill(Speaker& sp) {
    sp.pcm.resize(kFrameSamples);
    sp.pcmPos = 0;
    if (!sp.packets.empty()) {
        const auto& p = sp.packets.front();
        const int got = opus_decode(sp.dec, p.data(), (opus_int32)p.size(),
                                    sp.pcm.data(), kFrameSamples, 0);
        sp.packets.pop_front();
        if (got <= 0) memset(sp.pcm.data(), 0, sizeof(int16_t) * kFrameSamples);
        sp.concealed = 0;
        g_nPlayed.fetch_add(1);
        return true;
    }
    if (++sp.concealed > kMaxConceal) {
        sp.playing = false;
        sp.pcm.clear();
        return false;
    }
    const int got = opus_decode(sp.dec, nullptr, 0, sp.pcm.data(), kFrameSamples, 0);
    if (got <= 0) memset(sp.pcm.data(), 0, sizeof(int16_t) * kFrameSamples);
    g_nConcealed.fetch_add(1);
    return true;
}

// Per-speaker signal quality from the main thread: distance against the VHF
// horizon. Sessions come and go, so entries older than a while are dropped.
struct Quality { float q; std::chrono::steady_clock::time_point at; };
std::map<uint32_t, Quality> g_quality;          // guarded by g_mx

float qualityOf(uint32_t sid) {
    auto it = g_quality.find(sid);
    return it == g_quality.end() ? 1.f : it->second.q;
}

void render(int16_t* out, int count) {
    memset(out, 0, sizeof(int16_t) * (size_t)count);
    const float vol  = g_volume.load();
    const bool  fx   = g_filter.load();
    const float hiss = g_hiss.load();

    // Half duplex: while keyed you hear only your own sidetone, if enabled --
    // through the transmitter and the audio filter, as a headset would.
    if (g_tx.load()) {
        if (!g_sidetone.load()) return;
        std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
        if (!lk.owns_lock()) return;
        for (int i = 0; i < count && !g_sidechain.empty(); ++i) {
            float v = (float)g_sidechain.front();
            g_sidechain.pop_front();
            if (fx) v = Radio::amplifier(g_radio.filter(Radio::transmitter(g_radio.env, v)));
            v *= vol * 0.5f;                         // quieter than incoming
            out[i] = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
        }
        return;
    }

    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock()) return;                 // never stall the audio thread

    static std::vector<float> mix;
    mix.assign((size_t)count, 0.f);
    int   carriers = 0;
    float best = 0.f;                            // strongest signal among them

    for (auto& kv : g_speakers) {
        Speaker& sp = *kv.second;
        if (!sp.playing) {
            if ((int)sp.packets.size() < kPrebuffer) continue;
            sp.playing = true;
        }
        const float q = qualityOf(kv.first);
        const float knob = radioGain(sp.freqKhz);
        bool contributed = false;
        for (int i = 0; i < count; ++i) {
            if (sp.pcmPos >= sp.pcm.size()) {
                if (!refill(sp)) break;
                // Near the horizon the audio breaks up: whole 20 ms frames
                // go missing, more of them the further away they are.
                if (fx && q < 0.4f) {
                    g_radio.noise = g_radio.noise * 1664525u + 1013904223u;
                    const float roll = (float)(g_radio.noise >> 16) / 65536.f;
                    sp.dropped = roll < (0.4f - q) / 0.4f * 0.7f;
                } else {
                    sp.dropped = false;
                }
            }
            // Their transmitter, then the cockpit's volume knob for that radio.
            float smp = sp.dropped ? 0.f : (float)sp.pcm[sp.pcmPos];
            if (fx) smp = Radio::transmitter(sp.txEnv, smp);
            ++sp.pcmPos;
            mix[(size_t)i] += smp * knob;
            contributed = true;
        }
        if (contributed) { ++carriers; if (q > best) best = q; }
    }
    lk.unlock();

    const bool carrier = carriers > 0;
    if (!fx) {
        if (!carrier) return;
        for (int i = 0; i < count; ++i) {
            float v = mix[(size_t)i] * vol;
            out[i] = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
        }
        return;
    }

    g_radio.gate(carrier);
    if (!g_radio.active()) return;               // silence, and the filters stay put
    for (int i = 0; i < count; ++i) {
        const float speech = carrier ? mix[(size_t)i] : 0.f;
        float v = g_radio.receiver(speech, carrier, carriers, best, hiss) * vol;
        out[i] = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
    }
}

// Ask the backend what exists. Called on init and whenever the user opens
// the settings window's audio tab, since devices come and go.
void refreshDevices() {
    g_capDevs.clear();
    g_playDevs.clear();
    if (!g_ctxOk) return;

    ma_device_info* play = nullptr; ma_uint32 nPlay = 0;
    ma_device_info* cap  = nullptr; ma_uint32 nCap  = 0;
    if (ma_context_get_devices(&g_ctx, &play, &nPlay, &cap, &nCap) != MA_SUCCESS) return;

    for (ma_uint32 i = 0; i < nPlay; ++i) {
        g_playDevs.push_back({play[i].name, play[i].id, play[i].isDefault != 0});
    }
    for (ma_uint32 i = 0; i < nCap; ++i) {
        g_capDevs.push_back({cap[i].name, cap[i].id, cap[i].isDefault != 0});
    }
}

// nullptr means "system default", which is what an empty or unknown name gets.
const ma_device_id* findDevice(const std::vector<DevEntry>& list, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& d : list) {
        if (d.name == name) return &d.id;
    }
    return nullptr;
}

bool openDevices(std::string* err) {
    ma_device_config pc = ma_device_config_init(ma_device_type_playback);
    pc.playback.format    = ma_format_s16;
    pc.playback.channels  = 1;
    pc.playback.pDeviceID = (ma_device_id*)findDevice(g_playDevs, g_wantOut);
    pc.sampleRate         = kSampleRate;
    pc.periodSizeInFrames = kFrameSamples;
    pc.dataCallback       = playbackCb;
    if (ma_device_init(&g_ctx, &pc, &g_play) == MA_SUCCESS &&
        ma_device_start(&g_play) == MA_SUCCESS) {
        g_playOk = true;
        g_playName = g_play.playback.name;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format    = ma_format_s16;
    cfg.capture.channels  = 1;
    cfg.capture.pDeviceID = (ma_device_id*)findDevice(g_capDevs, g_wantMic);
    cfg.sampleRate        = kSampleRate;
    cfg.periodSizeInFrames = kFrameSamples;
    cfg.dataCallback      = captureCb;
    if (ma_device_init(&g_ctx, &cfg, &g_cap) == MA_SUCCESS &&
        ma_device_start(&g_cap) == MA_SUCCESS) {
        g_capOk = true;
        g_capName = g_cap.capture.name;
    }

    if (!g_playOk) {
        if (err) *err = "could not open a playback device";
        g_status = "no speakers";
        return false;
    }
    g_status = g_capOk ? ("OK  mic: " + g_capName) : "no microphone (receive only)";
    return true;
}

void captureCb(ma_device*, void*, const void* in, ma_uint32 n) {
    capture((const int16_t*)in, (int)n);
}

void playbackCb(ma_device*, void* out, const void*, ma_uint32 n) {
    render((int16_t*)out, (int)n);
}

}  // namespace

// --- public API ------------------------------------------------------------
bool init(Mode mode, std::string* err) {
    shutdown();
    g_mode = mode;

    int e = 0;
    g_enc = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &e);
    if (e != OPUS_OK || !g_enc) {
        if (err) *err = std::string("opus encoder: ") + opus_strerror(e);
        g_status = "codec failed";
        return false;
    }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(g_enc, OPUS_SET_DTX(0));

    g_radio = Radio();
    g_radio.design();

    if (mode == Mode::NoDevices) {
        g_status = "codec only";
        return true;
    }

    ma_backend nullOnly[] = {ma_backend_null};
    ma_context_config cc = ma_context_config_init();
    if (ma_context_init(mode == Mode::Null ? nullOnly : nullptr,
                        mode == Mode::Null ? 1 : 0, &cc, &g_ctx) != MA_SUCCESS) {
        if (err) *err = "no audio backend available";
        g_status = "no audio backend";
        return false;
    }
    g_ctxOk = true;
    refreshDevices();
    return openDevices(err);
}

bool init(Mode mode, const std::string& micName, const std::string& outName,
          std::string* err) {
    g_wantMic = micName;
    g_wantOut = outName;
    return init(mode, err);
}

std::vector<Device> listDevices(bool capture) {
    std::vector<Device> out;
    for (const auto& d : (capture ? g_capDevs : g_playDevs)) {
        out.push_back({d.name, d.isDefault});
    }
    return out;
}

bool reopenDevices(const std::string& micName, const std::string& outName,
                   std::string* err) {
    if (!g_ctxOk) {
        if (err) *err = "audio not initialised";
        return false;
    }
    const bool wasTx = g_tx.load();
    g_tx.store(false);

    if (g_capOk)  { ma_device_uninit(&g_cap);  g_capOk = false; }
    if (g_playOk) { ma_device_uninit(&g_play); g_playOk = false; }

    g_wantMic = micName;
    g_wantOut = outName;
    refreshDevices();
    const bool ok = openDevices(err);

    g_tx.store(wasTx);
    return ok;
}

const std::string& currentMic()    { return g_capName; }
const std::string& currentOutput() { return g_playName; }

void shutdown() {
    g_tx.store(false);
    if (g_capOk)  { ma_device_uninit(&g_cap);  g_capOk = false; }
    if (g_playOk) { ma_device_uninit(&g_play); g_playOk = false; }
    if (g_ctxOk)  { ma_context_uninit(&g_ctx); g_ctxOk = false; }
    if (g_enc)    { opus_encoder_destroy(g_enc); g_enc = nullptr; }
    std::lock_guard<std::mutex> lk(g_mx);
    g_speakers.clear();
    g_quality.clear();
    g_out.clear();
    g_sidechain.clear();
    g_capDevs.clear();
    g_playDevs.clear();
    g_status = "off";
}

bool available()      { return g_enc != nullptr && (g_playOk || g_mode == Mode::NoDevices); }
bool haveMicrophone() { return g_capOk; }

void setTransmitting(bool on) {
    if (on && !g_tx.load() && g_enc) opus_encoder_ctl(g_enc, OPUS_RESET_STATE);
    g_tx.store(on);
    if (!on) g_micLevel.store(0.f);
}
bool transmitting() { return g_tx.load(); }

void onIncomingFrame(uint32_t sid, uint16_t seq, const uint8_t* data, int len,
                     uint32_t freqKhz) {
    if (!data || len <= 0 || len > kMaxOpusBytes) return;
    std::lock_guard<std::mutex> lk(g_mx);
    auto found = g_speakers.find(sid);
    if (found == g_speakers.end() && (int)g_speakers.size() >= kMaxSpeakers) return;
    auto& slot = g_speakers[sid];
    if (!slot) {
        slot.reset(new Speaker());
        int e = 0;
        slot->dec = opus_decoder_create(kSampleRate, 1, &e);
        if (e != OPUS_OK) { g_speakers.erase(sid); return; }
    }
    Speaker& sp = *slot;
    if (freqKhz) sp.freqKhz = freqKhz;
    // Drop anything older than what we already have; sequence wraps at 65536.
    if (sp.haveSeq && (int16_t)(seq - sp.lastSeq) <= 0) return;
    sp.lastSeq = seq;
    sp.haveSeq = true;
    sp.lastRx  = std::chrono::steady_clock::now();
    sp.packets.emplace_back(data, data + len);
    while (sp.packets.size() > (size_t)kMaxJitter) sp.packets.pop_front();
    g_nReceived.fetch_add(1);
}

void pollOutgoing(std::vector<OutFrame>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(g_mx);
    while (!g_out.empty()) {
        out.push_back(std::move(g_out.front()));
        g_out.pop_front();
    }
}

void tick() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto it = g_quality.begin(); it != g_quality.end();) {
        if (std::chrono::duration<float>(now - it->second.at).count() > 10.f) {
            it = g_quality.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_speakers.begin(); it != g_speakers.end();) {
        const float age = std::chrono::duration<float>(now - it->second->lastRx).count();
        if (age > kSpeakerTimeoutS && it->second->packets.empty()) {
            it = g_speakers.erase(it);
        } else {
            ++it;
        }
    }
}

void  setVolume(float v) { g_volume.store(v < 0.f ? 0.f : (v > 1.f ? 1.f : v)); }
void  setSidetone(bool on) {
    g_sidetone.store(on);
    if (!on) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_sidechain.clear();
    }
}
void  setHiss(float level)     { g_hiss.store(level < 0.f ? 0.f : (level > 1.f ? 1.f : level)); }
void setRadioVolumes(uint32_t com1Khz, float com1Vol, uint32_t com2Khz, float com2Vol) {
    g_com1Khz.store(com1Khz);
    g_com2Khz.store(com2Khz);
    g_com1Vol.store(com1Vol < 0.f ? 0.f : (com1Vol > 1.f ? 1.f : com1Vol));
    g_com2Vol.store(com2Vol < 0.f ? 0.f : (com2Vol > 1.f ? 1.f : com2Vol));
}
float signalQualityOf(uint32_t sid) {
    std::lock_guard<std::mutex> lk(g_mx);
    return qualityOf(sid);
}
void  setSignalQuality(uint32_t sid, float q) {
    q = q < 0.f ? 0.f : (q > 1.f ? 1.f : q);
    std::lock_guard<std::mutex> lk(g_mx);
    g_quality[sid] = {q, std::chrono::steady_clock::now()};
}
void  setRadioFilter(bool on)  { g_filter.store(on); }
float micLevel()         { return g_micLevel.load(); }

std::vector<uint32_t> activeSpeakers() {
    std::vector<uint32_t> out;
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& kv : g_speakers) {
        if (kv.second->playing || !kv.second->packets.empty()) out.push_back(kv.first);
    }
    return out;
}

std::string status() { return g_status; }

Stats stats() {
    Stats s;
    s.framesEncoded   = g_nEncoded.load();
    s.framesReceived  = g_nReceived.load();
    s.framesPlayed    = g_nPlayed.load();
    s.framesConcealed = g_nConcealed.load();
    return s;
}

void testCapture(const int16_t* samples, int count) { capture(samples, count); }
void testRender(int16_t* out, int count)            { render(out, count); }

}  // namespace voice
}  // namespace xr

#endif  // XRADIO_USE_VOICE
