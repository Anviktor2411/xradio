// SimBrief in the plugin (Phase S3): fetch the pilot's latest OFP the official way
// (https://www.simbrief.com/api/xml.fetcher.php?username=<name>&json=1, or ?userid=<pilot id>; HTTP 400 =
// unknown user or no flight planned yet), read the few fields the flight-plan tab needs, and write the route as
// an X-Plane .fms file. Nothing is scraped; the fetch happens only when the pilot presses the button.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace xr {
namespace simbrief {

struct Fix { std::string ident; double lat = 0, lon = 0; int altFt = 0; std::string airway; bool sidStar = false; std::string type; };

struct Ofp {
    bool        ok = false;
    std::string error;                 // when !ok
    std::string callsign, airline, flightNumber, acType, reg, equip, selcal, wtc;
    std::string dep, dest, altn, depRwy, destRwy, sid, star, route, routeAtc;
    int         cruiseFt = 0, tas = 0;
    double      mach = 0;
    std::string eobt, eta, etd;        // HHMM zulu
    int         blockFuel = 0, tripFuel = 0, zfw = 0, tow = 0, ldw = 0;
    std::string units;                 // kg / lb
    std::string fpl, remarks;          // the ICAO flight plan string, item 18
    std::string pdfUrl, fmsUrl;
    std::vector<Fix> fixes;
    std::string summary() const;       // "TTR123 A320/M EETN-EVRA FL360 ULGOT N746 ABETI"
};

// Parse the OFP JSON (what xml.fetcher.php?json=1 returns). Pure, testable.
Ofp parse(const std::string& json);

// The fetch URL for a username or a numeric pilot id ("" when the user string is unusable).
std::string fetchUrl(const std::string& user);
// A clear message for an HTTP failure (400 = unknown user / no flight).
std::string explain(const std::string& httpError, const std::string& body);

// Start a fetch on a worker thread; the result arrives through poll() on the main thread.
void fetchAsync(const std::string& user);
bool poll(Ofp* out);                   // true once, when a fetch finished (ok or not)
bool busy();
void shutdown();                       // join the worker

// The route as an X-Plane 11/12 .fms (version 1100). Returns the text.
std::string toFms(const Ofp& ofp);
// Writes <xplaneRoot>/Output/FMS plans/<DEP><DEST>.fms; refuses to overwrite unless `overwrite`. Returns a note.
std::string writeFms(const Ofp& ofp, const std::string& xplaneRoot, bool overwrite, std::string* pathOut = nullptr);

}  // namespace simbrief
}  // namespace xr
