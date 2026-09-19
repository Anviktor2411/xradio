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
void shutdown() {}
bool available() { return false; }
bool haveMicrophone() { return false; }
void setTransmitting(bool) {}
bool transmitting() { return false; }
void onIncomingFrame(uint32_t, uint16_t, const uint8_t*, int) {}
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

// --- the "radio" sound: 300-3400 Hz band-pass ------------------------------
// Two second-order sections (RBJ cookbook), state kept on the playback thread.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    float run(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

Biquad g_hp, g_lp;

void designHighpass(Biquad& q, float fc) {
    const float w = 2.f * (float)kPi * fc / (float)kSampleRate;
    const float c = cosf(w), s = sinf(w), alpha = s / (2.f * 0.7071f);
    const float a0 = 1.f + alpha;
    q.b0 = (1.f + c) / 2.f / a0; q.b1 = -(1.f + c) / a0; q.b2 = (1.f + c) / 2.f / a0;
    q.a1 = -2.f * c / a0;        q.a2 = (1.f - alpha) / a0;
}

void designLowpass(Biquad& q, float fc) {
    const float w = 2.f * (float)kPi * fc / (float)kSampleRate;
    const float c = cosf(w), s = sinf(w), alpha = s / (2.f * 0.7071f);
    const float a0 = 1.f + alpha;
    q.b0 = (1.f - c) / 2.f / a0; q.b1 = (1.f - c) / a0; q.b2 = (1.f - c) / 2.f / a0;
    q.a1 = -2.f * c / a0;        q.a2 = (1.f - alpha) / a0;
}

uint32_t g_noise = 0x12345678u;
inline float hiss() {
    // cheap white noise: the carrier you hear when a squelch opens
    const float level = g_hiss.load();
    if (level <= 0.f) return 0.f;
    g_noise = g_noise * 1664525u + 1013904223u;
    return ((float)(g_noise >> 16) / 32768.f - 1.f) * 320.f * level;
}

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

void render(int16_t* out, int count) {
    memset(out, 0, sizeof(int16_t) * (size_t)count);

    // Half duplex: while keyed you hear only your own sidetone, if enabled.
    if (g_tx.load()) {
        if (!g_sidetone.load()) return;
        std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
        if (!lk.owns_lock()) return;
        const float vol = g_volume.load() * 0.5f;      // quieter than incoming
        for (int i = 0; i < count && !g_sidechain.empty(); ++i) {
            float v = (float)g_sidechain.front() * vol;
            g_sidechain.pop_front();
            if (g_filter.load()) v = g_lp.run(g_hp.run(v));
            out[i] = (int16_t)(v > 32767.f ? 32767.f : (v < -32768.f ? -32768.f : v));
        }
        return;
    }

    std::unique_lock<std::mutex> lk(g_mx, std::try_to_lock);
    if (!lk.owns_lock()) return;                 // never stall the audio thread

    static std::vector<int32_t> mix;
    mix.assign((size_t)count, 0);
    bool anyone = false;

    for (auto& kv : g_speakers) {
        Speaker& sp = *kv.second;
        if (!sp.playing) {
            if ((int)sp.packets.size() < kPrebuffer) continue;
            sp.playing = true;
        }
        for (int i = 0; i < count; ++i) {
            if (sp.pcmPos >= sp.pcm.size() && !refill(sp)) break;
            mix[(size_t)i] += sp.pcm[sp.pcmPos++];
        }
        anyone = true;
    }
    lk.unlock();

    if (!anyone) return;
    const float vol = g_volume.load();
    const bool filt = g_filter.load();
    for (int i = 0; i < count; ++i) {
        float v = (float)mix[(size_t)i] + hiss();
        if (filt) v = g_lp.run(g_hp.run(v));
        v *= vol;
        if (v > 32767.f) v = 32767.f;
        if (v < -32768.f) v = -32768.f;
        out[i] = (int16_t)v;
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

    designHighpass(g_hp, 300.f);
    designLowpass(g_lp, 3400.f);

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

void onIncomingFrame(uint32_t sid, uint16_t seq, const uint8_t* data, int len) {
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
