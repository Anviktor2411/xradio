// What aircraft is loaded, as everybody else should see it: the ICAO Doc 8643
// designator, its wake category, size and weight -- worked out from what the
// sim says, without anyone typing it in. No X-Plane in here: main.cpp reads the
// datarefs and the .acf path, this decides, tools/harness/acftype_test.cpp
// holds it to the add-ons that get it wrong.
//
//   1. the settings' "Aircraft type" field, when it is filled in, wins ("manual")
//   2. sim/aircraft/view/acf_ICAO, when it is a real Doc 8643 designator ("detected")
//   3. a known add-on recognised by its .acf file / folder, or a known wrong
//      value (Zibo and LevelUp 737s, ToLiss A319/A320/A321/A346, "737-800",
//      "A320neo"...) mapped to the right designator ("mapped")
//   4. nothing usable: "default" (C172 for the CSL matcher; shown as "type?")
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace xr {
namespace acftype {

struct Info {
    std::string icao;          // the designator everyone gets
    std::string source;        // "manual" | "detected" | "mapped" | "default"
    std::string note;          // why, for the window ("acf_ICAO was '737-800'")
    char        wtc = 0;       // 'L' 'M' 'H' 'J', 0 = unknown
    float       spanM = 0, lengthM = 0;
    int         mtowKg = 0;
    bool known() const { return source != "default"; }
};

inline std::string upperAlnum(const std::string& v, size_t maxLen = 7) {
    std::string out;
    for (char c : v) {
        if (isalnum((unsigned char)c)) out += (char)toupper((unsigned char)c);
        if (out.size() >= maxLen) break;
    }
    return out;
}
inline std::string lower(std::string v) {
    for (auto& c : v) c = (char)tolower((unsigned char)c);
    return v;
}

// ICAO Doc 8643 (the file XPMP2 ships in Resources/Doc8643.txt): manufacturer,
// model, designator, description, WTC -- tab separated.
struct Doc8643 {
    std::map<std::string, char> wtc;   // designator -> wake category
    bool loaded() const { return !wtc.empty(); }
    bool has(const std::string& icao) const { return wtc.count(icao) > 0; }
    char wtcOf(const std::string& icao) const {
        auto it = wtc.find(icao);
        return it == wtc.end() ? 0 : it->second;
    }
    void addLine(const std::string& line) {
        std::vector<std::string> f;
        size_t a = 0;
        while (true) {
            size_t b = line.find('\t', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f.size() < 5) return;
        std::string d = upperAlnum(f[2], 4);
        if (d.size() < 2 || d == "ZZZZ") return;
        std::string w = upperAlnum(f[4], 1);
        char c = w.empty() ? 0 : w[0];
        if (c == 'L' || c == 'M' || c == 'H' || c == 'J') {
            auto it = wtc.find(d);
            if (it == wtc.end() || c > it->second) wtc[d] = c;   // the heavier category of the variants
        } else if (!wtc.count(d)) {
            wtc[d] = 0;
        }
    }
    bool loadText(const std::string& text) {
        size_t a = 0;
        while (a < text.size()) {
            size_t b = text.find('\n', a);
            std::string line = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            addLine(line);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return loaded();
    }
    bool loadFile(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        std::string text;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
        return loadText(text);
    }
};

// Size and weight of the common types (span m, length m, MTOW kg, wake category).
struct Dims { float span, length; int mtow; char wtc; };
inline const std::map<std::string, Dims>& dims() {
    static const std::map<std::string, Dims> d = {
        {"A319", {35.8f, 33.8f, 75500, 'M'}}, {"A320", {35.8f, 37.6f, 78000, 'M'}}, {"A321", {35.8f, 44.5f, 93500, 'M'}},
        {"A19N", {35.8f, 33.8f, 75500, 'M'}}, {"A20N", {35.8f, 37.6f, 79000, 'M'}}, {"A21N", {35.8f, 44.5f, 97000, 'M'}},
        {"A332", {60.3f, 58.8f, 251000, 'H'}}, {"A333", {60.3f, 63.7f, 251000, 'H'}}, {"A339", {64.0f, 63.7f, 251000, 'H'}},
        {"A346", {63.5f, 75.4f, 380000, 'H'}}, {"A359", {64.8f, 66.8f, 283000, 'H'}}, {"A35K", {64.8f, 73.8f, 319000, 'H'}},
        {"A388", {79.8f, 72.7f, 575000, 'J'}}, {"B736", {35.8f, 31.2f, 65500, 'M'}}, {"B737", {35.8f, 33.6f, 70100, 'M'}},
        {"B738", {35.8f, 39.5f, 79016, 'M'}}, {"B739", {35.8f, 42.1f, 85100, 'M'}}, {"B37M", {35.9f, 35.6f, 80300, 'M'}},
        {"B38M", {35.9f, 39.5f, 82200, 'M'}}, {"B39M", {35.9f, 42.2f, 88300, 'M'}}, {"B752", {38.1f, 47.3f, 115680, 'M'}},
        {"B763", {47.6f, 54.9f, 186880, 'H'}}, {"B772", {60.9f, 63.7f, 297550, 'H'}}, {"B77W", {64.8f, 73.9f, 351530, 'H'}},
        {"B788", {60.1f, 56.7f, 227930, 'H'}}, {"B789", {60.1f, 62.8f, 254000, 'H'}}, {"B744", {64.4f, 70.7f, 396890, 'H'}},
        {"B748", {68.4f, 76.3f, 447700, 'H'}}, {"MD11", {51.7f, 61.2f, 285990, 'H'}}, {"CRJ2", {21.2f, 26.8f, 24040, 'M'}},
        {"CRJ9", {24.9f, 36.2f, 38330, 'M'}}, {"E170", {26.0f, 29.9f, 37200, 'M'}}, {"E175", {26.0f, 31.7f, 40370, 'M'}},
        {"E190", {28.7f, 36.2f, 51800, 'M'}}, {"E195", {28.7f, 38.7f, 52290, 'M'}}, {"BCS3", {35.1f, 38.7f, 70900, 'M'}},
        {"AT76", {27.1f, 27.2f, 23000, 'M'}}, {"DH8D", {28.4f, 32.8f, 29260, 'M'}}, {"C172", {11.0f, 8.3f, 1111, 'L'}},
        {"C182", {11.0f, 8.8f, 1406, 'L'}}, {"PA28", {10.7f, 7.3f, 1111, 'L'}}, {"SR22", {11.7f, 7.9f, 1633, 'L'}},
        {"DA40", {11.9f, 8.1f, 1310, 'L'}}, {"DA42", {13.4f, 8.6f, 1999, 'L'}}, {"BE58", {11.5f, 9.1f, 2495, 'L'}},
        {"C208", {15.9f, 11.5f, 3995, 'L'}}, {"TBM9", {12.8f, 10.7f, 3354, 'L'}}, {"PC12", {16.3f, 14.4f, 4740, 'L'}},
        {"C25C", {15.5f, 16.3f, 7761, 'L'}}, {"C700", {22.0f, 22.3f, 17010, 'M'}}, {"GLF6", {30.4f, 30.4f, 45200, 'M'}},
        {"S76", {13.4f, 16.0f, 5307, 'L'}}, {"EC35", {10.2f, 12.2f, 2910, 'L'}}, {"R22", {7.7f, 8.8f, 621, 'L'}},
        {"B06", {10.2f, 11.9f, 1451, 'L'}}, {"F18S", {13.6f, 18.3f, 29937, 'M'}}, {"A400", {42.4f, 45.1f, 141000, 'H'}},
    };
    return d;
}

// Known wrong / free-form acf_ICAO values and add-ons recognised by the .acf
// file name and folder. Checked against "<acf path> <acf_ICAO> <description>"
// in lower case; first match wins.
struct Rule { const char* needle; const char* icao; const char* why; };
inline const std::vector<Rule>& rules() {
    static const std::vector<Rule> r = {
        // LevelUp 737NG/MAX series: one family, the variant from the file name
        {"levelup", "", "LevelUp"},
        {"b737-800x", "B738", "Zibo 737-800"}, {"zibo", "B738", "Zibo 737-800"}, {"b738_4k", "B738", "Zibo 737-800"},
        {"737max8", "B38M", "737 MAX 8"}, {"737 max 8", "B38M", "737 MAX 8"}, {"737-8 max", "B38M", "737 MAX 8"}, {"b38m", "B38M", "737 MAX 8"},
        {"737max9", "B39M", "737 MAX 9"}, {"737max7", "B37M", "737 MAX 7"},
        {"737-900", "B739", "737-900"}, {"737-700", "B737", "737-700"}, {"737-600", "B736", "737-600"}, {"737-800", "B738", "737-800"},
        // ToLiss: the model from the file name
        {"a340-600", "A346", "ToLiss A340-600"}, {"a346", "A346", "ToLiss A340-600"},
        {"a321neo", "A21N", "A321neo"}, {"a321_xp", "A321", "ToLiss A321"}, {"a321", "A321", "A321"},
        {"a320neo", "A20N", "A320neo"}, {"a320_xp", "A320", "ToLiss A320"}, {"a320", "A320", "A320"},
        {"a319_xp", "A319", "ToLiss A319"}, {"a319", "A319", "A319"},
        {"a339", "A339", "A330-900"}, {"a330", "A333", "A330"}, {"a350", "A359", "A350"},
        {"b777-300", "B77W", "777-300ER"}, {"777-300", "B77W", "777-300ER"}, {"777", "B772", "777"},
        {"787-9", "B789", "787-9"}, {"787", "B788", "787"}, {"747-8", "B748", "747-8"}, {"747", "B744", "747-400"},
        {"757", "B752", "757"}, {"767", "B763", "767"}, {"md-11", "MD11", "MD-11"}, {"md11", "MD11", "MD-11"},
        {"crj-900", "CRJ9", "CRJ900"}, {"crj900", "CRJ9", "CRJ900"}, {"crj-200", "CRJ2", "CRJ200"},
        {"e-jet", "E175", "E-Jet"}, {"e175", "E175", "E175"}, {"e195", "E195", "E195"}, {"e190", "E190", "E190"},
        {"q400", "DH8D", "Dash 8 Q400"}, {"dash 8", "DH8D", "Dash 8"}, {"atr72", "AT76", "ATR 72"}, {"atr 72", "AT76", "ATR 72"},
        {"cessna_172", "C172", "Cessna 172 (default)"}, {"c172", "C172", "Cessna 172"}, {"172sp", "C172", "Cessna 172"},
        {"cessna_182", "C182", "Cessna 182"}, {"sr22", "SR22", "Cirrus SR22"}, {"da40", "DA40", "Diamond DA40"}, {"da62", "DA62", "Diamond DA62"},
        {"baron", "BE58", "Baron 58"}, {"caravan", "C208", "Caravan"}, {"tbm9", "TBM9", "TBM 900"}, {"tbm 9", "TBM9", "TBM 900"},
        {"pc-12", "PC12", "PC-12"}, {"citation x", "C750", "Citation X"}, {"s-76", "S76", "S-76"}, {"r22", "R22", "R22"},
        {"f-14", "F14", "F-14"}, {"f/a-18", "F18S", "F/A-18"},
    };
    return r;
}

inline std::string levelUpVariant(const std::string& hay) {
    if (hay.find("max") != std::string::npos) return hay.find("9") != std::string::npos && hay.find("max9") != std::string::npos ? "B39M" : "B38M";
    if (hay.find("900") != std::string::npos) return "B739";
    if (hay.find("700") != std::string::npos) return "B737";
    if (hay.find("600") != std::string::npos) return "B736";
    return "B738";
}

inline void fillDims(Info& i, const Doc8643* doc) {
    auto it = dims().find(i.icao);
    if (it != dims().end()) { i.spanM = it->second.span; i.lengthM = it->second.length; i.mtowKg = it->second.mtow; i.wtc = it->second.wtc; }
    if (doc && doc->wtcOf(i.icao)) i.wtc = doc->wtcOf(i.icao);
}

/** rawIcao: sim/aircraft/view/acf_ICAO; acfPath: the loaded .acf (XPLMGetNthAircraftModel);
 *  descrip: sim/aircraft/view/acf_descrip; manual: the settings field; doc: Doc 8643 (may be empty). */
inline Info resolve(const std::string& rawIcao, const std::string& acfPath, const std::string& descrip,
                    const std::string& manual, const Doc8643* doc) {
    Info i;
    const std::string man = upperAlnum(manual);
    if (!man.empty()) {
        i.icao = man; i.source = "manual";
        i.note = doc && doc->loaded() && !doc->has(man) ? "not in ICAO Doc 8643" : "";
        fillDims(i, doc);
        return i;
    }
    const std::string raw = upperAlnum(rawIcao, 8);
    const bool docOk = !doc || !doc->loaded();
    // a valid designator straight from the sim -- unless the add-on is one we know lies about it
    const std::string hay = lower(acfPath + " " + rawIcao + " " + descrip);
    if (raw.size() >= 2 && raw.size() <= 4 && (docOk ? dims().count(raw) > 0 : doc->has(raw))) {
        bool suspect = false;
        if (hay.find("levelup") != std::string::npos && raw != levelUpVariant(hay)) suspect = true;
        if (!suspect) {
            i.icao = raw; i.source = "detected";
            fillDims(i, doc);
            return i;
        }
    }
    for (const auto& r : rules()) {
        if (hay.find(r.needle) == std::string::npos) continue;
        i.icao = r.icao[0] ? r.icao : levelUpVariant(hay);
        i.source = "mapped";
        i.note = std::string(r.why) + (rawIcao.empty() ? " (acf_ICAO empty)" : " (acf_ICAO was '" + rawIcao + "')");
        fillDims(i, doc);
        return i;
    }
    // a four-character code that Doc 8643 does not know, but looks like one: take it, say so
    if (raw.size() >= 3 && raw.size() <= 4 && isalpha((unsigned char)raw[0])) {
        i.icao = raw; i.source = "detected"; i.note = "not in ICAO Doc 8643";
        fillDims(i, doc);
        return i;
    }
    i.icao = "C172"; i.source = "default"; i.note = rawIcao.empty() ? "the aircraft has no ICAO type" : "unknown type '" + rawIcao + "'";
    fillDims(i, doc);
    return i;
}

/** The airline of an airline callsign ("BTI123" -> "BTI"); "" for a registration ("ESABC", "N123AB"). */
inline std::string airlineOf(const std::string& callsign) {
    std::string c = upperAlnum(callsign, 10);
    if (c.size() < 4) return "";
    if (!(isalpha((unsigned char)c[0]) && isalpha((unsigned char)c[1]) && isalpha((unsigned char)c[2]))) return "";
    if (!isdigit((unsigned char)c[3])) return "";
    return c.substr(0, 3);
}

}  // namespace acftype
}  // namespace xr
