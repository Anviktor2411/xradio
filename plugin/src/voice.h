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
    bool     intercom = false;   // 0.7: for the cockpit intercom, not the radio (decided as it was encoded)
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

// Copies, not references: the microphone is opened on a thread of its own,
// so its name can change while the main thread is looking at it.
std::string currentMic();
std::string currentOutput();
void shutdown();
bool available();       // encoder + playback ready
// A microphone is there to use: open, opening, or waiting for the first PTT.
// False with none at all, or one that refused to open.
bool haveMicrophone();

// The microphone is not opened when the plugin loads. It opens the first time
// the pilot keys up, and closes again after ten minutes without a
// transmission, or straight away with this. Holding a Bluetooth headset's
// microphone switches the headset into its hands-free mode, which turns
// everything else the pilot hears -- the sim, other plugins, music -- into
// low-quality mono; nobody should pay that for a plugin they are not talking
// on. Main thread only.
void releaseMicrophone();

void setTransmitting(bool on);   // PTT
bool transmitting();

// --- 0.7: the cockpit intercom (docs/crew.md) -------------------------------
// The intercom's own push-to-talk. Frames encoded while it is down and the radio
// PTT is not come out of pollOutgoing() marked `intercom`; with both down the
// radio wins, as it does on a real audio panel. Talking on the intercom does not
// make the radio half duplex: you still hear the COMs.
void setIntercomTransmit(bool on);
bool intercomTransmitting();
// Incoming voice on kIntercomKhz is the intercom: its own volume, and none of the
// radio's squelch, noise, capture or break-up -- it is a wire, not a radio. It is
// heard while you transmit on the radio, too.
void setIntercomVolume(float v);   // 0..1
std::vector<uint32_t> activeIntercomSpeakers();

// Who is coming in right now, radio and intercom, for the window: the frequency,
// the signal and its level at the receiver.
struct SpeakerInfo {
    uint32_t sid = 0;
    uint32_t freqKhz = 0;
    float    quality = 1.f;      // 1 next door, 0 at the edge of coverage
    float    levelDbm = 0.f;
    bool     intercom = false;
};
std::vector<SpeakerInfo> speakers();

// --- 0.7: more of the radio sound, each one a switch ------------------------
void setSquelchTail(bool on);    // the burst and click as the squelch opens and closes (default on)
void setPttClick(bool on);       // a click in your own headset as you key and unkey (default off here;
                                 // the plugin's setting turns it on)
void setCrackle(bool on);        // static crashes on a weak signal, more of them towards the horizon
                                 // (default off here; on in the plugin's settings)

// --- 0.7: the microphone test ------------------------------------------------
// Hold to record (nothing is sent anywhere), let go to hear it back -- through
// the Opus codec and the whole radio chain, exactly as the others hear you. Six
// seconds at most; keying the radio or the intercom ends it. Main thread.
enum class MicTest { Idle, Recording, Playing };
constexpr float kMicTestMaxS = 6.f;
bool    startMicTest();          // false while transmitting, or without a codec
void    stopMicTest();           // recording -> playing; playing -> stopped
MicTest micTest();
float   micTestSeconds();        // recorded so far, or left to play

// A frame from another pilot, in the order the network delivered them.
// `freqKhz` is the frequency it came in on, so the cockpit's per-radio
// volume knob can be applied to it; 0 if unknown.
void onIncomingFrame(uint32_t sid, uint16_t seq, const uint8_t* data, int len,
                     uint32_t freqKhz = 0);

// The cockpit audio panel: what COM1 and COM2 are tuned to and how far up
// their volume knobs are (0..1). Incoming voice on a frequency one of them is
// tuned to is scaled by that knob, like it would be in the aircraft.
void setRadioVolumes(uint32_t com1Khz, float com1Vol, uint32_t com2Khz, float com2Vol);

// How loud a transmission on guard is when neither radio is tuned there.
// Everybody in range hears guard, but "everybody" still means everybody with
// a working radio -- an aeroplane with the avionics off hears nothing at all,
// so the caller passes 0 for that.
void setGuardGain(float gain);

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
// The squelch knob: how good a signal has to be (see setSignalQuality) before
// the receiver opens for it at all. A station below it is not heard and does
// not open the squelch, like a signal too weak for a real radio. 0 means the
// knob is all the way down: the squelch is open, there is constant static,
// and the faintest station gets through.
void  setSquelch(float minQuality);
// How much of the other pilot's cockpit comes through their microphone, 0..1.
// It goes through their transmitter's limiter like the voice does, so it is
// held down under the words and swells up between them, the way it does on a
// real transmission.
void  setCabinNoise(float level);
// How good another station's signal is, 1 next door down to 0 at the edge of
// its coverage: sets their noise level and how badly they break up. The main
// thread works it out from the radio model (radio.h, docs/radio.md); unknown
// pilots count as strong. `levelDbm` is what arrives at the receiver, which
// decides who wins when two transmit at once: more than 6 dB stronger and it
// captures the receiver, the other only a whine underneath; closer than that
// they garble each other. Without a level, one goes with the quality.
void  setSignalQuality(uint32_t sid, float quality);
void  setSignal(uint32_t sid, float quality, float levelDbm);
float micLevel();                // 0..1, peak of the most recent capture block
std::vector<uint32_t> activeSpeakers();
// Which frequency that pilot's voice is arriving on, 0 if they are not
// currently coming in. The window uses it to say where somebody is calling
// from when the pilot cannot see the radio log.
uint32_t speakerFreq(uint32_t sid);
std::string status();            // one line for the window
Stats stats();

// Test hooks: feed the capture path and pull from the playback path directly,
// as the audio devices would. Only meaningful in Mode::NoDevices.
void testCapture(const int16_t* samples, int count);
void testRender(int16_t* out, int count);
// The receiver's random numbers from a fixed start, so a test of the static is repeatable.
void testSeed(uint32_t seed);

}  // namespace voice
}  // namespace xr
