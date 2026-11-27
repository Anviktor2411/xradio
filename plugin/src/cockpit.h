// 0.7: the plain logic behind the shared cockpit, the quick-tune slots and the
// connection's round trip -- no X-Plane, no sockets, so tools/harness/cockpit_test.cpp
// can hold it to its word. main.cpp does the talking to the sim and the server.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "protocol.h"

namespace xr {
namespace cockpit {

// --- the radio panel ----------------------------------------------------------
// A frequency a COM radio can be tuned to: the air band, 8.33 kHz channels included.
inline bool validComKhz(uint32_t khz) { return khz >= 118000u && khz <= 136990u; }

// A panel from the network, made safe to write into the sim: frequencies out of
// the band become "leave alone" (0), volumes are clamped, a NaN is no volume.
inline CrewPanel sanitized(const CrewPanel& in) {
    CrewPanel p = in;
    auto khz = [](uint32_t& f) { if (!validComKhz(f)) f = 0; };
    khz(p.com1Khz); khz(p.com2Khz); khz(p.com1StbyKhz); khz(p.com2StbyKhz);
    if (p.txSel != TX_COM1 && p.txSel != TX_COM2) p.txSel = 0;
    if (p.rxMask != 0xFF) p.rxMask &= (uint8_t)(RX_COM1 | RX_COM2);
    auto vol = [](float& v) { if (!std::isfinite(v)) v = -1.f; else if (v > 1.f) v = 1.f; };
    vol(p.com1Vol); vol(p.com2Vol);
    return p;
}

// A CREW_TUNE that asks for nothing: every field "leave alone".
inline CrewPanel nothing() {
    CrewPanel p{};
    p.rxMask = 0xFF;
    p.com1Vol = p.com2Vol = -1.f;
    return p;
}

// What a copilot turned on their own panel since `before`, as a CREW_TUNE: the
// frequencies, the transmit selector and the receive buttons -- never the
// volumes, which belong to each crew member's own headset.
inline CrewPanel changes(const CrewPanel& before, const CrewPanel& now) {
    CrewPanel t = nothing();
    if (now.com1Khz != before.com1Khz && validComKhz(now.com1Khz)) t.com1Khz = now.com1Khz;
    if (now.com2Khz != before.com2Khz && validComKhz(now.com2Khz)) t.com2Khz = now.com2Khz;
    if (now.com1StbyKhz != before.com1StbyKhz && validComKhz(now.com1StbyKhz)) t.com1StbyKhz = now.com1StbyKhz;
    if (now.com2StbyKhz != before.com2StbyKhz && validComKhz(now.com2StbyKhz)) t.com2StbyKhz = now.com2StbyKhz;
    if (now.txSel != before.txSel && (now.txSel == TX_COM1 || now.txSel == TX_COM2)) t.txSel = now.txSel;
    if (now.rxMask != before.rxMask && now.rxMask != 0xFF) t.rxMask = (uint8_t)(now.rxMask & (RX_COM1 | RX_COM2));
    return t;
}

inline bool asksAnything(const CrewPanel& t) {
    return t.com1Khz || t.com2Khz || t.com1StbyKhz || t.com2StbyKhz || t.txSel || t.rxMask != 0xFF ||
           t.com1Vol >= 0.f || t.com2Vol >= 0.f;
}

// A CREW_TUNE applied to a panel: what it asks for, the rest as it was.
inline CrewPanel applied(const CrewPanel& panel, const CrewPanel& tune) {
    const CrewPanel t = sanitized(tune);
    CrewPanel p = panel;
    if (t.com1Khz) p.com1Khz = t.com1Khz;
    if (t.com2Khz) p.com2Khz = t.com2Khz;
    if (t.com1StbyKhz) p.com1StbyKhz = t.com1StbyKhz;
    if (t.com2StbyKhz) p.com2StbyKhz = t.com2StbyKhz;
    if (t.txSel) p.txSel = t.txSel;
    if (t.rxMask != 0xFF) p.rxMask = t.rxMask;
    if (t.com1Vol >= 0.f) p.com1Vol = t.com1Vol;
    if (t.com2Vol >= 0.f) p.com2Vol = t.com2Vol;
    return p;
}

// Whether the aircraft's panel already shows what a tune asked for -- until it
// does, a panel still on its way from before the tune must not undo the knob
// the copilot just turned.
inline bool sameTuning(const CrewPanel& a, const CrewPanel& b);
inline bool shows(const CrewPanel& panel, const CrewPanel& tune) {
    return sameTuning(applied(panel, tune), panel);
}

// The fields a crew member's sim follows -- not the volumes, not the power.
inline bool sameTuning(const CrewPanel& a, const CrewPanel& b) {
    return a.com1Khz == b.com1Khz && a.com2Khz == b.com2Khz && a.com1StbyKhz == b.com1StbyKhz &&
           a.com2StbyKhz == b.com2StbyKhz && a.txSel == b.txSel && a.rxMask == b.rxMask;
}

// The whole panel, for "has anything changed since it was last sent".
inline bool samePanel(const CrewPanel& a, const CrewPanel& b) {
    return sameTuning(a, b) && a.flags == b.flags && std::fabs(a.com1Vol - b.com1Vol) < 0.005f &&
           std::fabs(a.com2Vol - b.com2Vol) < 0.005f;
}

// --- words ------------------------------------------------------------------------
inline const char* roleName(uint8_t role) {
    switch (role) {
        case CREW_ROLE_PILOT:    return "pilot";
        case CREW_ROLE_COPILOT:  return "copilot";
        case CREW_ROLE_OBSERVER: return "observer";
        default:                 return "";
    }
}

inline uint8_t roleFromSetting(const std::string& s) {
    return s == "observer" ? (uint8_t)CREW_ROLE_OBSERVER : (uint8_t)CREW_ROLE_COPILOT;
}

// Why the server would not have us aboard, as the window says it.
inline std::string refusalText(uint8_t reason, const std::string& callsign) {
    const std::string who = callsign.empty() ? std::string("that pilot") : callsign;
    switch (reason) {
        case CREW_NO_SUCH:  return "nobody called " + who + " is in this flight";
        case CREW_NOT_OPEN: return who + " has not opened their cockpit to a crew";
        case CREW_BAD_CODE: return "the crew code does not match " + who + "'s";
        case CREW_FULL:     return who + "'s cockpit is full";
        case CREW_CLOSED:   return who + " closed the cockpit";
        case CREW_BUSY:     return "your own cockpit is open: set Shared cockpit to join first";
        default:            return "the server would not put you aboard";
    }
}

// --- quick-tune slots ----------------------------------------------------------
struct Preset {
    std::string label;      // up to 8 characters, as on the button
    uint32_t    khz = 0;
};
constexpr int kMaxPresets = 8;

// "118.100" / "118100" / "118.1" -> kHz, 0 if it is not a COM frequency.
inline uint32_t parseKhz(const std::string& text) {
    std::string t;
    for (char c : text) if (c != ' ') t += c;
    if (t.empty()) return 0;
    const char* s = t.c_str();
    char* end = nullptr;
    if (t.find('.') != std::string::npos) {
        const double mhz = strtod(s, &end);
        if (end == s || *end) return 0;
        const uint32_t khz = (uint32_t)std::lround(mhz * 1000.0);
        return validComKhz(khz) ? khz : 0;
    }
    const long v = strtol(s, &end, 10);
    if (end == s || *end || v <= 0) return 0;
    const uint32_t khz = (uint32_t)v;
    return validComKhz(khz) ? khz : 0;
}

inline std::string khzText(uint32_t khz) {
    char b[16];
    snprintf(b, sizeof b, "%u.%03u", (unsigned)(khz / 1000u), (unsigned)(khz % 1000u));
    return b;
}

// "TWR=118.100;GND=121.800;123.450" -- a label is optional; what is not a COM
// frequency is skipped; eight at most.
inline std::vector<Preset> parsePresets(const std::string& text) {
    std::vector<Preset> out;
    size_t a = 0;
    while (a <= text.size() && (int)out.size() < kMaxPresets) {
        size_t e = text.find_first_of(";,", a);
        const std::string item = text.substr(a, e == std::string::npos ? std::string::npos : e - a);
        Preset p;
        const size_t eq = item.find('=');
        std::string label = eq == std::string::npos ? "" : item.substr(0, eq);
        const std::string freq = eq == std::string::npos ? item : item.substr(eq + 1);
        p.khz = parseKhz(freq);
        std::string clean;
        for (char c : label) {
            if (c == ' ') continue;
            if ((c >= 'a' && c <= 'z')) c = (char)(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') clean += c;
            if (clean.size() >= 8) break;
        }
        p.label = clean.empty() && p.khz ? khzText(p.khz) : clean;
        if (p.khz) out.push_back(p);
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return out;
}

inline std::string formatPresets(const std::vector<Preset>& list) {
    std::string out;
    for (size_t i = 0; i < list.size() && (int)i < kMaxPresets; ++i) {
        if (!list[i].khz) continue;
        if (!out.empty()) out += ";";
        if (!list[i].label.empty() && list[i].label != khzText(list[i].khz)) out += list[i].label + "=";
        out += khzText(list[i].khz);
    }
    return out;
}

// --- the connection's round trip --------------------------------------------------
// Pings carry a counter; the pong brings it back. The last kWindow pings decide
// the loss, the answered ones the round trip (smoothed) and its jitter. A server
// from before 0.7 answers with an empty pong, which is matched to the one ping
// still waiting, if exactly one is.
class RttMeter {
public:
    static constexpr int kWindow = 30;
    static constexpr double kGiveUpS = 3.0;     // a ping unanswered this long is lost

    void sent(uint32_t seq, double now) {
        slots_[seq % kWindow] = Slot{seq, now, false, true};
        ++sentCount_;
    }
    // The pong for `seq` arrived at `now`. False for one that is not ours or not waiting.
    bool answered(uint32_t seq, double now) {
        Slot& s = slots_[seq % kWindow];
        if (!s.used || s.seq != seq || s.answered || now - s.at > kGiveUpS) return false;
        s.answered = true;
        take((now - s.at) * 1000.0);
        return true;
    }
    // An empty pong: the oldest ping still waiting, if it is the only one.
    bool answeredAnonymous(double now) {
        Slot* only = nullptr;
        int waiting = 0;
        for (Slot& s : slots_)
            if (s.used && !s.answered && now - s.at <= kGiveUpS) { ++waiting; only = &s; }
        if (waiting != 1 || !only) return false;
        only->answered = true;
        take((now - only->at) * 1000.0);
        return true;
    }
    bool   haveRtt() const { return samples_ > 0; }
    double rttMs() const { return rtt_; }
    double jitterMs() const { return jitter_; }
    // Lost share of the last pings that are old enough to have been answered.
    double lossPct(double now) const {
        int due = 0, lost = 0;
        for (const Slot& s : slots_) {
            if (!s.used || now - s.at < kGiveUpS) continue;
            ++due;
            if (!s.answered) ++lost;
        }
        return due ? 100.0 * lost / due : 0.0;
    }
    void reset() { *this = RttMeter(); }

private:
    struct Slot { uint32_t seq = 0; double at = 0; bool answered = false; bool used = false; };
    void take(double ms) {
        if (samples_ == 0) { rtt_ = ms; jitter_ = 0; }
        else {
            jitter_ += (std::fabs(ms - rtt_) - jitter_) / 8.0;   // RFC 3550's smoothing
            rtt_ += (ms - rtt_) / 8.0;
        }
        ++samples_;
    }
    Slot     slots_[kWindow];
    double   rtt_ = 0, jitter_ = 0;
    int      samples_ = 0;
    uint64_t sentCount_ = 0;
};

// Lost voice as a share, from the codec's counts over a window: frames the
// decoder had to make up (concealment) against frames it was given.
inline double voiceLossPct(uint64_t playedDelta, uint64_t concealedDelta) {
    const uint64_t all = playedDelta + concealedDelta;
    return all ? 100.0 * (double)concealedDelta / (double)all : 0.0;
}

}  // namespace cockpit
}  // namespace xr
