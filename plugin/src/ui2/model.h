// The new window's view-model (Phase U): everything the user interface shows, as plain data the plugin fills
// once per frame from its own state, and the actions the interface can ask for. No Dear ImGui and no X-Plane
// here, so the logic (formatting, validation, unread counts, toasts, the wizard's steps) is tested in
// tools/harness/ui_logic_test.cpp without either.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../settings.h"

namespace xr {
namespace ui2 {

struct Com {
    int         which = 1;
    uint32_t    activeKhz = 0, standbyKhz = 0;
    bool        powered = true;
    std::string stationName;          // "Tallinn Tower" for the active frequency, if a station is online there
    bool        rx = false, tx = false;
    float       signal = 0.f;         // 0..1 of the station being heard
    float       volume = 1.f;         // 0..1
};

struct Station {
    std::string callsign, kindName, icao;
    uint32_t    freqKhz = 0;
    double      distNm = -1.0;
    bool        ai = false, tuned = false, atis = false;
};

struct Pilot {
    uint32_t    sid = 0;
    std::string callsign, type;
    double      distNm = -1.0;
    float       altFt = 0.f, gsKt = 0.f;
    uint32_t    freqKhz = 0;          // 0 = unknown
    unsigned    squawk = 0;
    bool        talking = false, ai = false, drawn = true, emergency = false;
    float       signal = 0.f;
};

struct Message {
    double      t = 0;                // plugin seconds
    std::string from, text, timeText;
    uint32_t    freqKhz = 0;
    bool        atc = false, priv = false, mine = false, guard = false, system = false;
};

// The numbers of the imported SimBrief OFP (S3), as text ready to show.
struct OfpView {
    bool        present = false;
    std::string callsign, type, reg, dep, dest, altn, depRwy, destRwy, sid, star, route;
    std::string cruise, tas, mach, eobt, etd, eta, block, trip, zfw, tow, ldw, units, pdfUrl;
};

struct FlightPlanView {
    std::string rules = "I", dep, dest, altn, type, callsign, route, cruise, tas, eobt, equip, remarks;
    int         status = 0;           // 0 none, 1 filed, 2 acknowledged, 3 cleared, 4 cancelled
    unsigned    squawk = 0;
    std::string clearedSid, clearedLevel, clearedDest, clearance;   // what ATC answered, when known
    bool        simbriefAvailable = false;  // S3: an OFP was imported
    std::string simbriefSummary;
    bool        fetching = false;           // S3: a fetch is running
    int         ofpSerial = 0;              // bumps on every import, so the editable form reloads
    OfpView     ofp;
};

// The official page for planning a new flight; the plugin never scrapes it.
constexpr const char* kSimbriefNewFlightUrl = "https://www.simbrief.com/system/dispatch.php?newflight=1";

// One line of the plan-versus-clearance table.
struct ClearanceRow { std::string what, plan, atc; bool differs = false; };

struct HostInfo {
    bool        running = false, mapped = false, upnpBusy = false, doubleNat = false, upnpAsked = true;
    unsigned    port = 0;
    int         clients = 0;
    std::string address, joinCode, lanAddress, router, error;
    std::vector<std::string> callsigns;
};

struct Connection {
    bool        connected = false, connecting = false;
    std::string status, endpoint, callsign, icao, livery, skyLine;
    unsigned    protoLocal = 6, protoServer = 0;
    std::string versionMismatch;      // the message when the server speaks another version
    float       lastPacketS = -1.f;   // seconds since the last packet from the server
    int         packetsPerS = 0;
    int         peers = 0, aiCount = 0;
};

struct VoiceInfo {
    bool        available = false, haveMic = false;
    std::string status;
    float       micLevel = 0.f;
    std::vector<std::string> micDevices, outDevices;
};

struct UpdateInfo { bool newer = false; std::string latest, url, current; };

struct Toast {
    std::string text;
    int         colour = 0;           // 0 info, 1 amber (warning), 2 green (ok), 3 red (error)
    double      at = 0;
};

struct Model {
    double       elapsed = 0;
    Connection   conn;
    Com          com[2];
    bool         guard = false;       // 121.500 on COM2
    bool         pttDown = false;
    double       pttDownAt = -1;
    std::string  txWho;               // who is transmitting now (me, or a callsign)
    std::vector<Station> atc;
    std::vector<std::pair<std::string, std::string>> atis;   // "EETN information C" -> text, for tuned frequencies
    std::vector<Pilot>   pilots;
    std::vector<Message> messages;    // newest last
    FlightPlanView fp;
    HostInfo     host;
    VoiceInfo    voice;
    UpdateInfo   update;
    bool         towerActive = false;
    std::string  towerStatus;
    Settings     settings;            // the plugin's current settings (a copy)
    int          unreadMessages = 0;
    double       readUntil = 0;       // messages up to this time have been seen
};

struct Actions {
    std::function<void(int which, uint32_t khz)> tune;          // active frequency
    std::function<void(int which, uint32_t khz)> setStandby;
    std::function<void(int which)> swap;
    std::function<void(int which, float volume)> setVolume;
    std::function<void(bool on)> setGuard;
    std::function<void(const std::string& text)> sendText;      // "@CALLSIGN text" for a private message
    std::function<std::string()> fileFlightPlan;                // returns a note ("filed", or why not)
    std::function<void()> cancelFlightPlan;
    std::function<void(const Settings&)> applySettings;
    std::function<void()> reconnect;
    std::function<void()> refreshDevices;
    std::function<std::string()> copyAddress;                   // returns "copied" / why not
    std::function<void()> toggleTower;
    std::function<void(bool down)> keyPtt;
    std::function<void(const std::string& url)> openUrl;
    std::function<void()> importSimbrief;                       // S3
    std::function<std::string()> writeFms;                      // S3: returns a note
    std::function<void()> checkUpdate;
};

// ---------------------------------------------------------------- logic (tested without ImGui)
std::string fmtKhz(uint32_t khz);                       // 135905 -> "135.905"
uint32_t    parseFreq(const std::string& text);          // "135.905" / "135905" / "135.9" -> khz, 0 if not a COM frequency
bool        validCallsign(const std::string& cs);        // 2..10 of A-Z 0-9 -
std::string upperTrim(const std::string& s);
int         signalBars(float quality, int max = 5);      // 1..max
std::string stuckMicText(double downFor, double warnAfterS);   // "" or a warning
int         countUnread(const std::vector<Message>& msgs, double readUntil);
std::vector<const Pilot*> filterPilots(const std::vector<Pilot>& all, bool hideAi, const std::string& filter);
std::vector<const Station*> sortedStations(const std::vector<Station>& all);   // tuned first, then by distance

// Toasts live a few seconds; the overlay draws them even with the main window closed.
class Toasts {
public:
    void push(const std::string& text, int colour, double now);
    void expire(double now, double holdS = 8.0);
    const std::deque<Toast>& items() const { return items_; }
    size_t size() const { return items_.size(); }
    void clear() { items_.clear(); }
private:
    std::deque<Toast> items_;
};

// The first-run wizard: five short steps, with what blocks the next one (the SimBrief one never does).
struct Wizard {
    int step = 0;                     // 0 callsign/type, 1 audio, 2 push-to-talk, 3 host or join, 4 SimBrief (optional)
    static constexpr int kSteps = 5;
    static std::string blocker(int step, const Settings& s, const VoiceInfo& v);   // "" when the step is complete
    static const char* title(int step, const char* lang);
};

// The flight plan form's checks before filing.
std::vector<std::string> flightPlanProblems(const FlightPlanView& fp);
std::string levelText(int ft);                           // 36000 -> "FL360", 5000 -> "5000 ft", 0 -> ""
int         levelFeet(const std::string& s);             // "FL360" / "flight level 360" / "5000 ft" -> feet, 0 if none
// What a clearance text says: the SID ident, the level and the squawk ("cleared to Riga via ULGOT1G, climb 5000 feet, squawk 4521").
void        parseClearance(const std::string& text, std::string* sid, std::string* level, unsigned* squawk);
// The plan next to what ATC answered: squawk, SID, level, destination; `differs` only where both sides are known and disagree.
std::vector<ClearanceRow> clearanceRows(const FlightPlanView& fp);

}  // namespace ui2
}  // namespace xr
