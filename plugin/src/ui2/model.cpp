#include "model.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace xr {
namespace ui2 {

std::string fmtKhz(uint32_t khz) {
    if (!khz) return "---.---";
    char b[16];
    snprintf(b, sizeof b, "%u.%03u", khz / 1000, khz % 1000);
    return b;
}

uint32_t parseFreq(const std::string& text) {
    std::string t;
    for (char c : text) if (isdigit((unsigned char)c) || c == '.' || c == ',') t.push_back(c == ',' ? '.' : c);
    if (t.empty()) return 0;
    uint32_t khz = 0;
    if (t.find('.') != std::string::npos) {
        const double mhz = atof(t.c_str());
        khz = (uint32_t)(mhz * 1000.0 + 0.5);
    } else if (t.size() == 6) {
        khz = (uint32_t)atoi(t.c_str());
    } else if (t.size() == 5) {
        khz = (uint32_t)atoi(t.c_str()) * 10;             // 13590 -> 135.900
    } else if (t.size() == 3) {
        khz = (uint32_t)atoi(t.c_str()) * 1000;
    } else return 0;
    // 8.33 kHz raster: the last digit must be 0 or 5
    if (khz < 118000 || khz > 136990) return 0;
    const uint32_t r = khz % 25;
    if (r != 0 && r != 5 && r != 10 && r != 15 && r != 20) return 0;
    return khz;
}

std::string upperTrim(const std::string& s) {
    std::string o;
    for (char c : s) if (!isspace((unsigned char)c)) o.push_back((char)toupper((unsigned char)c));
    return o;
}

bool validCallsign(const std::string& cs) {
    if (cs.size() < 2 || cs.size() > 10) return false;
    for (char c : cs) if (!(isalnum((unsigned char)c) || c == '-')) return false;
    return isalnum((unsigned char)cs[0]) != 0;
}

int signalBars(float quality, int max) {
    if (quality < 0.f) quality = 0.f;
    if (quality > 1.f) quality = 1.f;
    return 1 + (int)(quality * (float)(max - 1) + 0.5f);
}

std::string stuckMicText(double downFor, double warnAfterS) {
    if (downFor < warnAfterS) return "";
    char b[80];
    snprintf(b, sizeof b, "microphone keyed for %d s -- stuck?", (int)downFor);
    return b;
}

int countUnread(const std::vector<Message>& msgs, double readUntil) {
    int n = 0;
    for (const auto& m : msgs) if (!m.mine && !m.system && m.t > readUntil) ++n;
    return n;
}

std::vector<const Pilot*> filterPilots(const std::vector<Pilot>& all, bool hideAi, const std::string& filter) {
    const std::string f = upperTrim(filter);
    std::vector<const Pilot*> out;
    for (const auto& p : all) {
        if (hideAi && p.ai) continue;
        if (!f.empty() && upperTrim(p.callsign).find(f) == std::string::npos && upperTrim(p.type).find(f) == std::string::npos) continue;
        out.push_back(&p);
    }
    std::sort(out.begin(), out.end(), [](const Pilot* a, const Pilot* b) {
        if (a->talking != b->talking) return a->talking;
        const double da = a->distNm < 0 ? 1e9 : a->distNm, db = b->distNm < 0 ? 1e9 : b->distNm;
        return da < db;
    });
    return out;
}

std::vector<const Station*> sortedStations(const std::vector<Station>& all) {
    std::vector<const Station*> out;
    for (const auto& s : all) out.push_back(&s);
    std::sort(out.begin(), out.end(), [](const Station* a, const Station* b) {
        if (a->tuned != b->tuned) return a->tuned;
        if (a->atis != b->atis) return !a->atis;
        const double da = a->distNm < 0 ? 1e9 : a->distNm, db = b->distNm < 0 ? 1e9 : b->distNm;
        return da < db;
    });
    return out;
}

void Toasts::push(const std::string& text, int colour, double now) {
    if (text.empty()) return;
    if (!items_.empty() && items_.back().text == text && now - items_.back().at < 2.0) { items_.back().at = now; return; }
    items_.push_back({text, colour, now});
    while (items_.size() > 5) items_.pop_front();
}

void Toasts::expire(double now, double holdS) {
    while (!items_.empty() && now - items_.front().at > holdS) items_.pop_front();
}

std::string Wizard::blocker(int step, const Settings& s, const VoiceInfo& v) {
    switch (step) {
        case 0:
            if (!validCallsign(upperTrim(s.callsign))) return "a callsign: 2 to 10 letters or digits";
            if (upperTrim(s.acIcao).size() < 2) return "the aircraft type (ICAO, e.g. C172)";
            return "";
        case 1:
            if (!v.available) return "no audio device could be opened -- pick another one, or carry on without voice";
            return "";
        case 2:
            if (s.pttKey <= 0) return "a push-to-talk key (click the field and press a key), or bind the command xradio/ptt in X-Plane";
            return "";
        default:
            if (!s.hostEnabled && upperTrim(s.host).empty()) return "a server address or join code, or host the flight yourself";
            return "";
    }
}

const char* Wizard::title(int step, const char* lang) {
    const bool et = lang && lang[0] == 'e' && lang[1] == 't';
    switch (step) {
        case 0: return et ? "Kes sa oled" : "Who you are";
        case 1: return et ? "Heli" : "Audio";
        case 2: return et ? "Saatenupp" : "Push-to-talk";
        default: return et ? "Lend" : "The flight";
    }
}

std::vector<std::string> flightPlanProblems(const FlightPlanView& fp) {
    std::vector<std::string> out;
    const std::string dep = upperTrim(fp.dep), dest = upperTrim(fp.dest);
    if (dep.size() != 4) out.push_back("departure: a 4-letter ICAO code");
    if (dest.size() != 4) out.push_back("destination: a 4-letter ICAO code");
    if (!fp.altn.empty() && upperTrim(fp.altn).size() != 4) out.push_back("alternate: a 4-letter ICAO code or empty");
    const std::string r = upperTrim(fp.rules);
    if (r != "I" && r != "V" && r != "Y" && r != "Z") out.push_back("rules: I, V, Y or Z");
    const int cr = atoi(fp.cruise.c_str());
    if (!fp.cruise.empty() && (cr < 100 || cr > 60000)) out.push_back("cruise: feet (3500) or a flight level as feet (36000)");
    if (upperTrim(fp.type).size() < 2) out.push_back("aircraft type");
    return out;
}

}  // namespace ui2
}  // namespace xr
