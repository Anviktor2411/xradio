// The window's logic without the window (Phase U): frequency formatting and parsing, callsign checks,
// signal bars, the stuck-mic warning, unread counts, pilot filtering and station ordering, the toast queue,
// the first-run wizard's blockers and the flight-plan form's checks. No ImGui, no X-Plane.
//   ./ui_logic_test
#include <cstdio>
#include <string>

#include "../../plugin/src/ui2/model.h"

static int fails = 0;
static void check(bool ok, const char* what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; }

int main() {
    using namespace xr::ui2;
    check(fmtKhz(135905) == "135.905" && fmtKhz(118000) == "118.000" && fmtKhz(0) == "---.---", "fmtKhz");
    check(parseFreq("135.905") == 135905 && parseFreq("135905") == 135905 && parseFreq("118,0") == 118000 && parseFreq("121.5") == 121500 && parseFreq("13590") == 135900,
          "parseFreq: dotted, plain, comma, short forms");
    check(parseFreq("117.000") == 0 && parseFreq("137.0") == 0 && parseFreq("135.903") == 0 && parseFreq("abc") == 0 && parseFreq("") == 0, "parseFreq refuses out-of-band and off-raster values");
    check(parseFreq("135.905") % 25 == 5 && parseFreq("135.910") == 135910 && parseFreq("135.915") == 135915, "8.33 kHz raster digits accepted");
    check(validCallsign("ESXYZ") && validCallsign("N1234A") && validCallsign("FIN321") && !validCallsign("X") && !validCallsign("TOO-LONG-CALL") && !validCallsign("BAD CS") && !validCallsign("-ABC"), "validCallsign");
    check(upperTrim(" es xyz ") == "ESXYZ", "upperTrim");
    check(signalBars(0.f) == 1 && signalBars(1.f) == 5 && signalBars(0.5f) == 3 && signalBars(2.f) == 5, "signalBars 1..5");
    check(stuckMicText(10, 30).empty() && stuckMicText(45, 30).find("45 s") != std::string::npos, "stuck-mic warning after the limit");

    std::vector<Message> msgs = {{10, "EETN_TWR", "cleared", "", 135905, true, false, false, false, false}, {20, "ME", "wilco", "", 135905, false, false, true, false, false},
                                 {30, "FIN321", "hello", "", 135905, false, true, false, false, false}, {40, "", "connected", "", 0, false, false, false, false, true}};
    check(countUnread(msgs, 0) == 2 && countUnread(msgs, 25) == 1 && countUnread(msgs, 100) == 0, "countUnread skips my own and system lines");

    std::vector<Pilot> pilots = {{1, "FIN321", "A320", 12.0, 3000, 250, 135905, 4521, false, false, true, false, 0.5f}, {2, "AIR01", "B738", 3.0, 100, 10, 0, 7000, true, true, true, false, 0.9f},
                                 {3, "SAS123", "A321", 30.0, 35000, 450, 0, 7700, false, false, true, true, 0.f}};
    auto f1 = filterPilots(pilots, false, "");
    check(f1.size() == 3 && f1[0]->callsign == "AIR01", "filterPilots: the talking one first, then by distance");
    auto f2 = filterPilots(pilots, true, "");
    check(f2.size() == 2 && f2[0]->callsign == "FIN321", "filterPilots: hide AI");
    auto f3 = filterPilots(pilots, false, "sas");
    check(f3.size() == 1 && f3[0]->callsign == "SAS123", "filterPilots: text filter, case-insensitive");
    auto f4 = filterPilots(pilots, false, "A32");
    check(f4.size() == 2, "filterPilots matches the type too");

    std::vector<Station> st = {{"EETN_APP", "Approach", "EETN", 127905, 20.0, false, false, false}, {"EETN_ATIS", "ATIS", "EETN", 124880, 5.0, false, false, true},
                               {"EETN_TWR", "Tower", "EETN", 135905, 5.0, true, true, false}, {"EETN_GND", "Ground", "EETN", 131905, 5.0, false, false, false}};
    auto s1 = sortedStations(st);
    check(s1[0]->callsign == "EETN_TWR" && s1[1]->callsign == "EETN_GND" && s1[2]->callsign == "EETN_APP" && s1[3]->atis, "sortedStations: tuned first, ATIS last, then distance");

    Toasts t;
    t.push("ATC: cleared for takeoff", 2, 1.0);
    t.push("ATC: cleared for takeoff", 2, 1.5);
    t.push("connection lost", 3, 2.0);
    check(t.size() == 2, "a repeated toast within two seconds merges");
    t.expire(9.6, 8.0);
    check(t.size() == 1 && t.items().front().text == "connection lost", "toasts expire after the hold time");
    for (int i = 0; i < 10; ++i) t.push("x" + std::to_string(i), 0, 20.0 + i * 3);
    check(t.size() == 5, "at most five toasts are kept");

    xr::Settings s;
    VoiceInfo v;
    check(!Wizard::blocker(0, s, v).empty(), "wizard step 1 needs a callsign");
    s.callsign = "ESXYZ"; s.acIcao = "C172";
    check(Wizard::blocker(0, s, v).empty(), "wizard step 1 done with callsign and type");
    check(!Wizard::blocker(1, s, v).empty(), "wizard step 2 flags missing audio");
    v.available = true;
    check(Wizard::blocker(1, s, v).empty() && !Wizard::blocker(2, s, v).empty(), "wizard step 3 needs a PTT key");
    s.pttKey = 0x20; s.host.clear(); s.hostEnabled = false;
    check(Wizard::blocker(2, s, v).empty() && !Wizard::blocker(3, s, v).empty(), "wizard step 4 needs a server or hosting");
    s.host = "K7M2Q-X4PB9";
    check(Wizard::blocker(3, s, v).empty(), "a join code satisfies the last step");
    check(std::string(Wizard::title(0, "et")) == "Kes sa oled" && std::string(Wizard::title(0, "en")) == "Who you are", "wizard titles in both languages");

    FlightPlanView fp;
    fp.dep = "EETN"; fp.dest = "EVRA"; fp.type = "A320"; fp.rules = "I"; fp.cruise = "36000";
    check(flightPlanProblems(fp).empty(), "a complete flight plan has no problems");
    fp.dest = "EVR"; fp.rules = "Q"; fp.cruise = "99";
    auto pr = flightPlanProblems(fp);
    check(pr.size() == 3, "destination, rules and cruise are checked");

    // S3: the clearance text versus the plan
    check(levelText(36000) == "FL360" && levelText(5000) == "5000 ft" && levelText(0).empty(), "levelText");
    check(levelFeet("FL360") == 36000 && levelFeet("flight level 70") == 7000 && levelFeet("5000 ft") == 5000 && levelFeet("") == 0, "levelFeet");
    {
        std::string sid, lvl; unsigned sq = 0;
        parseClearance("TTR123, cleared to Riga via ULGOT1G departure, climb 5000 feet, squawk 4521", &sid, &lvl, &sq);
        check(sid == "ULGOT1G" && lvl == "5000 ft" && sq == 4521, "parseClearance: SID, initial climb, squawk");
        parseClearance("FIN321 cleared EVRA ABETI2A departure flight level 360 squawk 1234", &sid, &lvl, &sq);
        check(sid == "ABETI2A" && lvl == "FL360" && sq == 1234, "parseClearance: flight level, a callsign with digits is not a SID");
        parseClearance("roger, squawk 7000", &sid, &lvl, &sq);
        check(sid.empty() && lvl.empty() && sq == 7000, "parseClearance: nothing invented");
    }
    {
        FlightPlanView c;
        c.dest = "EVRA"; c.cruise = "36000"; c.route = "ULGOT1G ULGOT N746 ABETI"; c.squawk = 4521;
        c.clearedSid = "ULGOT1G"; c.clearedLevel = "5000 ft"; c.clearedDest = "EVRA";
        auto rows = clearanceRows(c);
        check(rows.size() == 4 && rows[0].atc == "4521" && rows[1].plan == "ULGOT1G" && !rows[1].differs && !rows[2].differs && !rows[3].differs, "clearanceRows: a matching clearance has no differences");
        c.clearedSid = "ULGOT2W"; c.clearedLevel = "FL340"; c.clearedDest = "EYVI";
        rows = clearanceRows(c);
        check(rows[1].differs && rows[2].differs && rows[3].differs, "clearanceRows: another SID, cruise level and destination are marked");
        c.ofp.present = true; c.ofp.sid = "ULGOT2W"; rows = clearanceRows(c);
        check(!rows[1].differs && rows[1].plan == "ULGOT2W", "clearanceRows: the OFP's SID wins over the route's first token");
    }

    printf(fails ? "%d FAILED\n" : "ui_logic_test OK\n", fails);
    return fails ? 1 : 0;
}
