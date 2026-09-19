// Voice over the radio: microphone -> Opus -> network -> Opus -> speakers.
//
// Threads involved:
//   * miniaudio capture thread   encodes 20 ms frames while the PTT is down
//   * miniaudio playback thread  decodes + mixes every active speaker
//   * the plugin's network thread moves frames to and from the server
//   * the main thread            reads status for the window, runs tick()
//
// Everything crosses between them through one mutex with microsecond-sized
// critical sections; the audio callbacks only ever try_lock so they can never
// stall on the network side.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xr {
namespace voice {

// 48 kHz mono, 20 ms per frame -- Opus' native VoIP configuration.
constexpr int kSampleRate = 48000;
constexpr int kFrameSamples = 960;

enum class Mode {
    Real,       // open the system's default microphone and speakers
    Null,       // miniaudio's null backend: silent devices that still run
    NoDevices,  // codec and buffers only; tests drive the callbacks by hand
};

struct OutFrame {
    uint16_t seq;
    std::vector<uint8_t> opus;
};

struct Stats {
    uint64_t framesEncoded = 0;
    uint64_t framesReceived = 0;
    uint64_t framesPlayed = 0;
    uint64_t framesConcealed = 0;   // packet-loss concealment ran
};

// A capture or playback device the system offers.
struct Device {
    std::string name;
    bool        isDefault = false;
};

// Opens devices and codec. `micName` / `outName` select a device by the name
// listDevices() reported; empty means the system default. On failure `err`
// says why and the plugin keeps working without voice.
bool init(Mode mode, std::string* err);
bool init(Mode mode, const std::string& micName, const std::string& outName,
          std::string* err);

// What the system offers right now. Empty before init(), or on the null
// backend. Safe to call from the main thread while audio is running.
std::vector<Device> listDevices(bool capture);

// Reopen just the audio devices, keeping the codec and any active streams.
// Used when the settings window picks a different microphone or output.
bool reopenDevices(const std::string& micName, const std::string& outName,
                   std::string* err);

const std::string& currentMic();
const std::string& currentOutput();
void shutdown();
bool available();       // encoder + playback ready
bool haveMicrophone();  // capture device opened

void setTransmitting(bool on);   // PTT
bool transmitting();

// A frame from another pilot, in the order the network delivered them.
// `freqKhz` is the frequency it came in on, so the cockpit's per-radio
// volume knob can be applied to it; 0 if unknown.
void onIncomingFrame(uint32_t sid, uint16_t seq, const uint8_t* data, int len,
                     uint32_t freqKhz = 0);

// The cockpit audio panel: what COM1 and COM2 are tuned to and how far up
// their volume knobs are (0..1). Incoming voice on a frequency one of them is
// tuned to is scaled by that knob, like it would be in the aircraft.
void setRadioVolumes(uint32_t com1Khz, float com1Vol, uint32_t com2Khz, float com2Vol);

// Signal quality last set for a pilot (1 if never set): for the window's bars.
float signalQualityOf(uint32_t sid);

// Take frames the encoder has produced; the caller sends them.
void pollOutgoing(std::vector<OutFrame>& out);

// Main-thread housekeeping: forgets speakers that stopped talking.
void tick();

void  setVolume(float v);        // 0..1
void  setSidetone(bool on);      // hear your own voice while keyed
void  setHiss(float level);      // 0..1, scales every bit of noise the radio makes
void  setRadioFilter(bool on);   // the whole radio sound: limiter, overdrive,
                                 // squelch, 300-2700 Hz filter. Off = clean audio.
// How good another pilot's signal is, 1 next door down to 0 at the VHF
// horizon: sets their noise level and how badly they break up. The main
// thread works it out from distance; unknown pilots count as strong.
void  setSignalQuality(uint32_t sid, float quality);
float micLevel();                // 0..1, peak of the most recent capture block
std::vector<uint32_t> activeSpeakers();
std::string status();            // one line for the window
Stats stats();

// Test hooks: feed the capture path and pull from the playback path directly,
// as the audio devices would. Only meaningful in Mode::NoDevices.
void testCapture(const int16_t* samples, int count);
void testRender(int16_t* out, int count);

}  // namespace voice
}  // namespace xr
