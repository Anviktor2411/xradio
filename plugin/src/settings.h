// Everything the settings window can change, plus its config-file format.
//
// One descriptor table (describe()) drives both the file I/O and the UI, so a
// new setting is added in exactly one place and cannot end up saved but not
// shown, or shown but not saved.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "joincode.h"

namespace xr {

struct Settings {
    // --- connection ---
    std::string host       = "127.0.0.1";
    std::string port       = "49100";      // text so the field can be cleared
    std::string callsign   = "XRADIO1";
    std::string acIcao;                    // empty: read from the aircraft you are flying
    std::string password;                  // flight password: needed to join, required to host
    bool        autoConnect = true;
    // One request to GitHub's releases page at startup, so a pilot on an old
    // build learns about it before a flight refuses them. Off means nothing
    // is sent anywhere.
    bool        checkUpdates = true;
    float       reportHz   = 5.f;          // position reports per second
    float       smoothMs   = 350.f;        // interpolation delay

    // --- audio ---
    std::string micDevice;                 // empty = system default
    std::string outDevice;
    float       volume     = 0.8f;
    bool        sidetone   = false;        // hear yourself while keyed
    float       hiss       = 0.35f;        // every bit of noise the radio makes
    bool        radioFilter = true;        // the whole radio sound; off = clean audio
    // With the window closed a pilot sees nothing at all: no radio log, no
    // sign that anybody called. A small notice in the corner is the whole of
    // what they get, so it is on by default.
    bool        popUps = true;
    int         pttKey     = 0;            // keyboard key that keys the radio; 0 = none

    // --- hosting ---
    // With this on the plugin runs the relay server itself and connects to
    // it locally, so nobody has to set a server up. The host/port above are
    // then unused -- the friends type this machine's address instead.
    bool        hostEnabled = false;
    std::string hostPort    = "49100";
    bool        hostUpnp    = true;        // ask the router to open the port
    // One sim decides the sky for everyone who wants it. Which one is settled
    // by the server, first come: on a flight hosted in X-Plane that is
    // naturally the host, but on a dedicated server -- which has no weather of
    // its own -- it is whoever connected first with this switched on. Both
    // halves are opt-out: a pilot who has set up their own weather keeps it.
    bool        shareWeather  = true;      // "mine can be the flight's"
    bool        followWeather = true;      // "give me the flight's"

    // --- traffic ---
    bool        showTraffic = true;
    bool        showLabels  = true;
    float       labelDistNm = 20.f;
    float       trafficRangeNm = 80.f;
    // Blank means: our own Resources/CSL, and if that is empty, whatever CSL
    // library another traffic plugin already installed. A path here overrides
    // that search.
    std::string cslPath = "";

    int         port_i() const { return atoi(port.c_str()); }
    int         hostPort_i() const { return atoi(hostPort.c_str()); }

    // The host field may hold a join code ("K7M2Q-X4PB9") instead of an
    // address; then the port field is ignored and both come from the code.
    bool        hostIsCode() const { return joincode::looksLikeCode(host); }

    // Where the client should actually connect: hosting means our own server.
    std::string activeHost() const {
        if (hostEnabled) return "127.0.0.1";
        std::string ip; uint16_t p = 0;
        if (hostIsCode() && joincode::decode(host, &ip, &p)) return ip;
        return host;
    }
    int activePort() const {
        if (hostEnabled) return hostPort_i();
        std::string ip; uint16_t p = 0;
        if (hostIsCode() && joincode::decode(host, &ip, &p)) return p;
        return port_i();
    }
};

// --- descriptor table -------------------------------------------------------
enum class Kind { Text, Bool, Slider, Choice, KeyBind };

struct FieldRef {
    const char* key;       // config-file key
    const char* label;     // shown in the window
    Kind        kind;
    int         tab;       // 0 connection, 1 audio, 2 traffic, 3 hosting
    void*       ptr;       // &Settings member
    float       lo = 0.f, hi = 1.f;
    const char* unit = "";
    int         decimals = 0;
    size_t      maxLen = 63;
    bool        digitsOnly = false;
    // Choice fields get their option list from the caller at draw time (the
    // audio device lists), so there is nothing to store here.
};

inline std::vector<FieldRef> describe(Settings& s) {
    return {
        {"host",      "Server host or join code", Kind::Text, 0, &s.host, 0,0,"",0, 63},
        {"port",      "Port",           Kind::Text,   0, &s.port,       0,0,"",0,  5, true},
        {"callsign",  "Callsign",       Kind::Text,   0, &s.callsign,   0,0,"",0, 15},
        {"actype",    "Aircraft type (blank = from the sim)", Kind::Text, 0, &s.acIcao, 0,0,"",0, 7},
        {"password",  "Flight password", Kind::Text,  0, &s.password,   0,0,"",0, 31},
        {"checkupdates", "Tell me about new versions", Kind::Bool, 0, &s.checkUpdates},
        {"autoconnect", "Connect on startup", Kind::Bool, 0, &s.autoConnect},
        {"reporthz",  "Report rate",    Kind::Slider, 0, &s.reportHz,   1.f, 10.f, " Hz", 0},
        {"smoothms",  "Smoothing delay", Kind::Slider, 0, &s.smoothMs, 100.f, 1000.f, " ms", 0},

        {"mic",       "Microphone",     Kind::Choice, 1, &s.micDevice},
        {"speakers",  "Output",         Kind::Choice, 1, &s.outDevice},
        {"volume",    "Volume",         Kind::Slider, 1, &s.volume,     0.f, 1.f, "%", 0},
        {"sidetone",  "Hear own voice", Kind::Bool,   1, &s.sidetone},
        {"hiss",      "Radio noise",    Kind::Slider, 1, &s.hiss,       0.f, 1.f, "%", 0},
        {"radiofilter", "Radio sound (limiter, squelch, filter)", Kind::Bool, 1, &s.radioFilter},
        {"pttkey",    "Push-to-talk key", Kind::KeyBind, 1, &s.pttKey},
        {"popups",    "Show messages when the window is closed", Kind::Bool, 1,
                      &s.popUps},

        {"showtraffic", "Draw other aircraft", Kind::Bool, 2, &s.showTraffic},
        {"showlabels",  "Callsign labels",     Kind::Bool, 2, &s.showLabels},
        {"labeldist",   "Label range",   Kind::Slider, 2, &s.labelDistNm,   1.f, 100.f, " nm", 0},
        {"range",       "Traffic range", Kind::Slider, 2, &s.trafficRangeNm, 5.f, 200.f, " nm", 0},
        {"cslpath",     "CSL folder (blank = find one)", Kind::Text, 2, &s.cslPath,
                        0, 0, "", 0, 255},
        {"shareweather",  "Offer my weather and time to the flight", Kind::Bool, 2,
                          &s.shareWeather},
        {"followweather", "Fly the flight's weather and time", Kind::Bool, 2,
                          &s.followWeather},

        {"hosting",     "Host a flight here", Kind::Bool, 3, &s.hostEnabled},
        {"hostport",    "Port to host on",    Kind::Text, 3, &s.hostPort, 0,0,"",0, 5, true},
        {"hostupnp",    "Ask the router to open it", Kind::Bool, 3, &s.hostUpnp},
    };
}

inline const char* tabName(int i) {
    switch (i) {
        case 0:  return "Connection";
        case 1:  return "Audio";
        case 2:  return "Traffic";
        default: return "Hosting";
    }
}
inline constexpr int kNumTabs = 4;

// The name of an X-Plane virtual key, for the window. Letters and digits are
// their ASCII; the rest are the handful people actually bind.
inline std::string keyName(int vk) {
    if (vk <= 0) return "none";
    if ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A)) return std::string(1, (char)vk);
    if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x70 + 1);
    if (vk >= 0x60 && vk <= 0x69) return "Numpad " + std::to_string(vk - 0x60);
    switch (vk) {
        case 0x08: return "Backspace";  case 0x09: return "Tab";       case 0x0D: return "Enter";
        case 0x1B: return "Escape";     case 0x20: return "Space";     case 0x21: return "Page Up";
        case 0x22: return "Page Down";  case 0x23: return "End";       case 0x24: return "Home";
        case 0x25: return "Left";       case 0x26: return "Up";        case 0x27: return "Right";
        case 0x28: return "Down";       case 0x2D: return "Insert";    case 0x2E: return "Delete";
        case 0x6A: return "Numpad *";   case 0x6B: return "Numpad +";  case 0x6C: return "Numpad Enter";
        case 0x6D: return "Numpad -";   case 0x6E: return "Numpad .";  case 0x6F: return "Numpad /";
        case 0xA0: return "Left Shift"; case 0xA1: return "Right Shift";
        case 0xA2: return "Left Ctrl";  case 0xA3: return "Right Ctrl";
        case 0xA4: return "Left Alt";   case 0xA5: return "Right Alt";
        case 0xB5: return ";";  case 0xB6: return "=";  case 0xB7: return ",";  case 0xB8: return "-";
        case 0xB9: return ".";  case 0xBA: return "`";  case 0xBB: return "/";  case 0xBC: return "[";
        case 0xBD: return "\\"; case 0xBE: return "]";  case 0xBF: return "'";
        default: break;
    }
    return "key " + std::to_string(vk);
}

// --- file I/O ---------------------------------------------------------------
namespace detail {

inline std::string trim(const std::string& v) {
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace detail

inline bool saveSettings(const Settings& s, const std::string& path) {
    Settings copy = s;                       // describe() needs a non-const ref
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "# XRadio configuration\n");
    int lastTab = -1;
    for (const auto& fld : describe(copy)) {
        if (fld.tab != lastTab) {
            fprintf(f, "\n# %s\n", tabName(fld.tab));
            lastTab = fld.tab;
        }
        switch (fld.kind) {
            case Kind::Text:
            case Kind::Choice:
                fprintf(f, "%s = %s\n", fld.key, ((std::string*)fld.ptr)->c_str());
                break;
            case Kind::Bool:
                fprintf(f, "%s = %s\n", fld.key, *(bool*)fld.ptr ? "yes" : "no");
                break;
            case Kind::Slider:
                fprintf(f, "%s = %g\n", fld.key, (double)*(float*)fld.ptr);
                break;
            case Kind::KeyBind:
                fprintf(f, "%s = %d\n", fld.key, *(int*)fld.ptr);
                break;
        }
    }
    fclose(f);
    return true;
}

inline bool loadSettings(Settings& s, const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    auto fields = describe(s);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string t = detail::trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = detail::trim(t.substr(0, eq));
        const std::string v = detail::trim(t.substr(eq + 1));

        for (auto& fld : fields) {
            if (k != fld.key) continue;
            switch (fld.kind) {
                case Kind::Text:
                case Kind::Choice:
                    *(std::string*)fld.ptr = v.substr(0, fld.maxLen);
                    break;
                case Kind::Bool:
                    *(bool*)fld.ptr = (v == "yes" || v == "true" || v == "1" || v == "on");
                    break;
                case Kind::Slider:
                    *(float*)fld.ptr = detail::clampf((float)atof(v.c_str()), fld.lo, fld.hi);
                    break;
                case Kind::KeyBind: {
                    const int k = atoi(v.c_str());
                    *(int*)fld.ptr = (k < 0 || k > 255) ? 0 : k;
                    break;
                }
            }
            break;
        }
    }
    fclose(f);
    return true;
}

// Anything the user could have hand-edited into nonsense.
inline std::string validate(const Settings& s) {
    if (detail::trim(s.callsign).empty())  return "Callsign cannot be empty";
    if (s.hostEnabled) {
        const int hp = s.hostPort_i();
        if (hp < 1 || hp > 65535)          return "Host port must be 1-65535";
        return "";                         // the server field is unused while hosting
    }
    if (detail::trim(s.host).empty())      return "Host cannot be empty";
    if (s.hostIsCode()) {
        std::string ip; uint16_t p = 0;
        if (!joincode::decode(s.host, &ip, &p)) return "That join code is not right -- check the letters";
        return "";
    }
    const int p = s.port_i();
    if (p < 1 || p > 65535)                return "Port must be 1-65535";
    return "";
}

}  // namespace xr
