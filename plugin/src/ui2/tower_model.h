// Tower mode's windows, the parts that are logic and not drawing: which bay a strip sits in (TowerTrainer's own
// rules from strips.js, so the plugin's board and the trainer's agree), the command line's history and callsign
// completion, the ground radar's projection and zoom, the connection banner's words, what a light signal means
// and whose line a log entry is. No Dear ImGui and no X-Plane: tools/harness/ui_logic_test.cpp tests all of it.
#pragma once

#include <string>
#include <vector>

#include "../tower.h"

namespace xr {
namespace ui2 {
namespace tw {

// ---------------------------------------------------------------- strips
std::vector<std::string> baysFor(const std::string& pos, bool split);   // the bays of a position's board, in order
const char* bayTitle(const std::string& bay);                           // "holding" -> "Holding"
std::string autoBay(const tower::Aircraft& a);                          // the bay its state puts it in
std::string bayOf(const tower::Aircraft& a);                            // ... unless a controller moved it, in this state
bool incomingFor(const tower::Aircraft& a, const std::string& pos);     // handed to this position, not yet accepted
bool onBoard(const tower::Aircraft& a, const std::string& pos, bool split);
std::string shortState(const tower::Aircraft& a);                       // "Clr req", "Taxiing" -- the trainer's labels.js

// ---------------------------------------------------------------- the command line
// What selecting an aircraft does to the line: its callsign in front of whatever is typed, unless an instruction
// to someone else is already half written (more than a callsign).
std::string withCallsign(const std::string& cmd, const std::string& cs);
// Callsigns the first word could become, best first (prefix matches, then anywhere in the callsign).
std::vector<std::string> callsignMatches(const std::string& cmd, const std::vector<tower::Aircraft>& ac, size_t max = 6);
std::string completeCallsign(const std::string& cmd, const std::vector<tower::Aircraft>& ac);   // Tab; unchanged when nothing fits

class History {
public:
    void push(const std::string& line);                 // newest last; a repeat of the last one is not stored twice
    bool up(const std::string& current, std::string* out);   // one older (the line being typed is kept as the draft)
    bool down(std::string* out);                        // one newer; past the newest gives the draft back
    size_t size() const { return items_.size(); }
private:
    std::vector<std::string> items_;
    int pos_ = -1;                                      // -1: not browsing
    std::string draft_;
};

// ---------------------------------------------------------------- ground radar
struct Vec { float x = 0, y = 0; };
struct View { float cx = 0, cy = 0, ppm = 0.15f; };   // centre in the trainer's metres, pixels per metre; north up

Vec   toScreen(const View& v, Vec origin, Vec size, float x, float y);
Vec   toWorld(const View& v, Vec origin, Vec size, float sx, float sy);
void  zoomAt(View& v, Vec origin, Vec size, float factor, Vec screen);   // the point under the mouse stays put
void  pan(View& v, float dxPixels, float dyPixels);                       // the picture follows the mouse
View  fit(const tower::State& s, Vec size);                               // the whole airport in view
float ringStep(float ppm);            // range ring spacing in metres: round, and ~70 px or more apart
std::string rangeText(float metres);  // "500 m", "2 km", "5 nm"
float wingspanM(const std::string& wtc);
const std::vector<Vec>& planeShape(); // an aircraft seen from above, nose to +y, 1 unit = the wingspan
// How much of its label a target gets -- TowerTrainer's own label filter (render.js), so a busy apron stays
// readable: 2 = the callsign and a second line, 1 = the callsign alone, 0 = the symbol only. Service vehicles
// show theirs on hover; parked aircraft keep to the callsign zoomed out, and to nothing further out still.
int labelLines(const tower::Aircraft& a, bool selected, bool hovered, bool calling, float ppm, bool labelsOn);
// Who gets the free room first: the selected aircraft, then anything calling or in trouble, airborne traffic,
// aircraft moving on the ground, parked ones, vehicles.
int labelPriority(const tower::Aircraft& a, bool selected, bool calling);
// A label that finds no free room is left out (1 for these) instead of covering another one.
bool labelMayHide(const tower::Aircraft& a, bool selected, bool calling);

// ---------------------------------------------------------------- the connection banner
struct Banner {
    bool        show = false;
    int         level = 0;            // 0 info, 1 warning, 2 problem
    std::string title, detail;
    bool        retry = false;        // offer "Retry now"
};
Banner bannerFor(const tower::State& s, const std::string& code);

// ---------------------------------------------------------------- the light gun
struct Signal { const char* id; const char* label; float r, g, b; bool flashing; };
const std::vector<Signal>& signals();
const char* signalMeaning(const std::string& id, bool onGround);   // what it tells a pilot, by the book

// ---------------------------------------------------------------- the frequency log
enum class Sender { System, Atc, AiAtc, Departure, Arrival, LivePilot, Vehicle, Pilot, Problem };
Sender senderOf(const tower::LogEntry& e);
// Text the window's font can show: emoji and the like dropped, a few symbols put in words.
std::string displayText(const std::string& utf8);
// A live call the trainer's log now carries (so it is not shown twice).
bool inLog(const tower::LiveCall& c, const std::vector<tower::LogEntry>& log);

std::string waitingText(int seconds);  // "45 s", "3 min"
std::string levelText(const std::string& cfl);   // "4000" -> "4000 ft", "F180" / "FL180" -> "FL180"

}  // namespace tw
}  // namespace ui2
}  // namespace xr
