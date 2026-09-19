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

namespace xr {

struct Settings {
    // --- connection ---
    std::string host       = "127.0.0.1";
    std::string port       = "49100";      // text so the field can be cleared
    std::string callsign   = "XRADIO1";
    std::string acIcao     = "C172";
    bool        autoConnect = true;
    float       reportHz   = 5.f;          // position reports per second
    float       smoothMs   = 350.f;        // interpolation delay

    // --- audio ---
    std::string micDevice;                 // empty = system default
    std::string outDevice;
    float       volume     = 0.8f;
    bool        sidetone   = false;        // hear yourself while keyed
    float       hiss       = 0.35f;        // carrier noise
    bool        radioFilter = true;        // 300-3400 Hz band-pass

    // --- hosting ---
    // With this on the plugin runs the relay server itself and connects to
    // it locally, so nobody has to set a server up. The host/port above are
    // then unused -- the friends type this machine's address instead.
    bool        hostEnabled = false;
    std::string hostPort    = "49100";
    bool        hostUpnp    = true;        // ask the router to open the port

    // --- traffic ---
    bool        showTraffic = true;
    bool        showLabels  = true;
    float       labelDistNm = 20.f;
    float       trafficRangeNm = 80.f;

    int         port_i() const { return atoi(port.c_str()); }
    int         hostPort_i() const { return atoi(hostPort.c_str()); }

    // Where the client should actually connect: hosting means our own server.
    std::string activeHost() const { return hostEnabled ? "127.0.0.1" : host; }
    int         activePort() const { return hostEnabled ? hostPort_i() : port_i(); }
};

// --- descriptor table -------------------------------------------------------
enum class Kind { Text, Bool, Slider, Choice };

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
        {"host",      "Server host",    Kind::Text,   0, &s.host,       0,0,"",0, 63},
        {"port",      "Port",           Kind::Text,   0, &s.port,       0,0,"",0,  5, true},
        {"callsign",  "Callsign",       Kind::Text,   0, &s.callsign,   0,0,"",0, 15},
        {"actype",    "Aircraft type",  Kind::Text,   0, &s.acIcao,     0,0,"",0,  7},
        {"autoconnect", "Connect on startup", Kind::Bool, 0, &s.autoConnect},
        {"reporthz",  "Report rate",    Kind::Slider, 0, &s.reportHz,   1.f, 10.f, " Hz", 0},
        {"smoothms",  "Smoothing delay", Kind::Slider, 0, &s.smoothMs, 100.f, 1000.f, " ms", 0},

        {"mic",       "Microphone",     Kind::Choice, 1, &s.micDevice},
        {"speakers",  "Output",         Kind::Choice, 1, &s.outDevice},
        {"volume",    "Volume",         Kind::Slider, 1, &s.volume,     0.f, 1.f, "%", 0},
        {"sidetone",  "Hear own voice", Kind::Bool,   1, &s.sidetone},
        {"hiss",      "Carrier hiss",   Kind::Slider, 1, &s.hiss,       0.f, 1.f, "%", 0},
        {"radiofilter", "Radio filter (300-3400 Hz)", Kind::Bool, 1, &s.radioFilter},

        {"showtraffic", "Draw other aircraft", Kind::Bool, 2, &s.showTraffic},
        {"showlabels",  "Callsign labels",     Kind::Bool, 2, &s.showLabels},
        {"labeldist",   "Label range",   Kind::Slider, 2, &s.labelDistNm,   1.f, 100.f, " nm", 0},
        {"range",       "Traffic range", Kind::Slider, 2, &s.trafficRangeNm, 5.f, 200.f, " nm", 0},

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
    const int p = s.port_i();
    if (p < 1 || p > 65535)                return "Port must be 1-65535";
    return "";
}

}  // namespace xr
