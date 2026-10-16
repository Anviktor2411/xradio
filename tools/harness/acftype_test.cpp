// What aircraft is loaded (acftype.h) and the pilot's own TCAS (tcasread.h), without X-Plane:
// sim/aircraft/view/acf_ICAO and the .acf path as the add-ons really set them, ICAO Doc 8643, the airline
// from the callsign, and the X-Plane 12 TCAS datarefs turned into the PT_TCAS record.
//   ./acftype_test [lib/XPMP2/Resources/Doc8643.txt]
#include <cmath>
#include <cstdio>
#include <string>

#include "acftype.h"
#include "tcasread.h"

using namespace xr;

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}
static std::string show(const acftype::Info& i) { return i.icao + " (" + i.source + ") " + i.note; }

int main(int argc, char** argv) {
    acftype::Doc8643 doc;
    const std::string path = argc > 1 ? argv[1] : "lib/XPMP2/Resources/Doc8643.txt";
    if (!doc.loadFile(path)) {
        // a few lines in the file's own format, so the test runs from anywhere
        doc.loadText("AIRBUS\tA-320\tA320\tL2J\tM\nAIRBUS\tA-320neo\tA20N\tL2J\tM\nAIRBUS\tA-319\tA319\tL2J\tM\nAIRBUS\tA-321\tA321\tL2J\tM\n"
                     "AIRBUS\tA-340-600\tA346\tL4J\tH\nBOEING\t737-800\tB738\tL2J\tM\nBOEING\t737 MAX 8\tB38M\tL2J\tM\n"
                     "CESSNA\t172 Skyhawk\tC172\tL1P\tL\nAIRBUS\tA-380-800\tA388\tL4J\tJ\n(ANY)\tAircraft type not (yet) assigned a designator\tZZZZ\t-\t-\n");
        printf("  (Doc8643.txt not found at %s: using the built-in lines)\n", path.c_str());
    }
    printf("ICAO Doc 8643\n");
    check("B738 is a designator, medium", doc.has("B738") && doc.wtcOf("B738") == 'M');
    check("A346 heavy, A388 super", doc.wtcOf("A346") == 'H' && doc.wtcOf("A388") == 'J');
    check("ZZZZ and made-up codes are not", !doc.has("ZZZZ") && !doc.has("B7X8"));

    printf("\ndetection: acf_ICAO, the .acf path, add-ons that get it wrong\n");
    auto r = acftype::resolve("B738", "Aircraft/Boeing 737-800/b738.acf", "Boeing 737-800", "", &doc);
    check("a valid acf_ICAO is taken as detected", r.icao == "B738" && r.source == "detected" && r.wtc == 'M' && std::fabs(r.spanM - 35.8f) < 0.1f, show(r));
    r = acftype::resolve("", "Aircraft/B737-800X/b738_4k.acf", "Boeing 737-800X", "", &doc);
    check("Zibo 737 with an empty acf_ICAO -> B738 (mapped)", r.icao == "B738" && r.source == "mapped", show(r));
    r = acftype::resolve("737-800", "Aircraft/Zibo/B737-800X/b738.acf", "", "", &doc);
    check("'737-800' typed into acf_ICAO -> B738", r.icao == "B738" && r.source == "mapped" && r.note.find("737-800") != std::string::npos, show(r));
    r = acftype::resolve("B738", "Aircraft/LevelUp 737NG Series/737MAX8/LevelUp_737MAX8.acf", "Boeing 737 MAX 8", "", &doc);
    check("LevelUp 737 MAX 8 saying B738 -> B38M", r.icao == "B38M" && r.source == "mapped", show(r));
    r = acftype::resolve("", "Aircraft/ToLiss A319/a319_XP11.acf", "Airbus A319", "", &doc);
    check("ToLiss A319 -> A319", r.icao == "A319", show(r));
    r = acftype::resolve("A320", "Aircraft/ToLiss A320 Neo/a320_XP11.acf", "A320neo", "", &doc);
    check("ToLiss A320 (acf_ICAO A320) stays A320", r.icao == "A320" && r.source == "detected", show(r));
    r = acftype::resolve("A20N", "Aircraft/ToLiss A320 Neo/a320neo.acf", "", "", &doc);
    check("A20N detected as A20N", r.icao == "A20N", show(r));
    r = acftype::resolve("A321-200", "Aircraft/ToLiss A321/a321_XP11.acf", "", "", &doc);
    check("ToLiss A321 with 'A321-200' -> A321", r.icao == "A321", show(r));
    r = acftype::resolve("", "Aircraft/ToLiss A340-600/a340-600_XP11.acf", "", "", &doc);
    check("ToLiss A340-600 -> A346, heavy", r.icao == "A346" && r.wtc == 'H', show(r));
    r = acftype::resolve("C172", "Aircraft/Laminar Research/Cessna 172 SP/Cessna_172SP.acf", "Cessna 172SP", "", &doc);
    check("default C172 -> C172, light", r.icao == "C172" && r.source == "detected" && r.wtc == 'L', show(r));
    r = acftype::resolve("", "Aircraft/Laminar Research/Cessna 172 SP/Cessna_172SP.acf", "", "", &doc);
    check("default Cessna with no acf_ICAO -> C172 (mapped)", r.icao == "C172" && r.source == "mapped", show(r));
    r = acftype::resolve("", "Aircraft/Homebuilt/thing.acf", "My thing", "", &doc);
    check("nothing to go on -> default, flagged", r.source == "default" && !r.known(), show(r));
    r = acftype::resolve("B738", "Aircraft/Boeing 737-800/b738.acf", "", "a21n", &doc);
    check("the settings field overrides (manual)", r.icao == "A21N" && r.source == "manual", show(r));

    printf("\nthe type changes when another aircraft is loaded (XPLM_MSG_PLANE_LOADED)\n");
    auto a = acftype::resolve("C172", "Aircraft/Laminar Research/Cessna 172 SP/Cessna_172SP.acf", "", "", &doc);
    auto b = acftype::resolve("", "Aircraft/B737-800X/b738_4k.acf", "", "", &doc);
    check("C172 -> B738 after the plane-loaded re-detect", a.icao == "C172" && b.icao == "B738" && a.icao != b.icao);

    printf("\nairline from the callsign (CSL matching)\n");
    check("BTI123 -> BTI, DLH45A -> DLH", acftype::airlineOf("BTI123") == "BTI" && acftype::airlineOf("DLH45A") == "DLH");
    check("registrations have none", acftype::airlineOf("ESABC").empty() && acftype::airlineOf("N123AB").empty() && acftype::airlineOf("GABCD").empty());

    printf("\nthe pilot's TCAS (X-Plane 12 datarefs)\n");
    const float inf = 3.40282e38f;
    float climbBands[6] = {-inf, 1500, 1500, inf, 0, 0};    // red below +1500, green above
    auto t = tcas::read(2, tcas::M_CLIMB, climbBands, nullptr, 0);
    check("RA climb: level 2, sense +1, target 1500 fpm", t.level == 2 && t.sense == 1 && std::fabs(t.vsTargetMs - 1500 * tcas::kFpmToMs) < 0.05f);
    float descBands[6] = {0, 0, -inf, -1500, -1500, inf};
    t = tcas::read(2, tcas::M_DESCEND, descBands, nullptr, 0);
    check("RA descend: sense -1, target -1500 fpm", t.sense == -1 && std::fabs(t.vsTargetMs + 1500 * tcas::kFpmToMs) < 0.05f);
    t = tcas::read(2, tcas::M_INCREASE_CLIMB, nullptr, nullptr, 0);
    check("increase climb without bands: 2500 fpm", t.sense == 1 && std::fabs(t.vsTargetMs - 2500 * tcas::kFpmToMs) < 0.05f);
    t = tcas::read(1, tcas::M_TRAFFIC, nullptr, nullptr, 0);
    check("TA: level 1, no sense", t.level == 1 && t.sense == 0);
    t = tcas::read(0, tcas::M_CLEAR, nullptr, nullptr, 0, 2, 1);
    check("clear of conflict after an RA: level 3", t.level == 3);
    t = tcas::read(2, tcas::M_NONE, nullptr, nullptr, 0, 2, -1);
    check("between aural messages the sense is kept", t.sense == -1);
    int threats[5] = {0, 2, 4, 1, -1};
    t = tcas::read(2, tcas::M_DESCEND, nullptr, threats, 5);
    check("the intruder is the target with the corrective RA", t.threatIndex == 2);
    tcas::State s1 = tcas::read(2, tcas::M_CLIMB, nullptr, nullptr, 0), s2 = tcas::read(2, tcas::M_DESCEND, nullptr, nullptr, 0);
    check("a reversal is a change worth sending", s1 != s2);

    printf("\n%s\n", failures ? "FAILURES" : "ALL PASSED");
    return failures ? 1 : 0;
}
