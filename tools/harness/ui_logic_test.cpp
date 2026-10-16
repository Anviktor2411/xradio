// The window's logic without the window (Phase U): frequency formatting and parsing, callsign checks,
// signal bars, the stuck-mic warning, unread counts, pilot filtering and station ordering, the toast queue,
// the first-run wizard's blockers and the flight-plan form's checks, and Tower mode's strip bays, command line,
// radar maths and banner (ui2/tower_model.h). No ImGui, no X-Plane.
//   ./ui_logic_test
#include <cmath>
#include <cstdio>
#include <string>

#include "../../plugin/src/ui2/i18n.h"
#include "../../plugin/src/ui2/model.h"
#include "../../plugin/src/ui2/tower_model.h"

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
    check(Wizard::kSteps == 5 && std::string(Wizard::title(4, "en")) == "SimBrief (optional)" && Wizard::blocker(4, s, v).empty(), "wizard step 5 is SimBrief and never blocks");

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

    {   // Tower mode's windows: the logic under them (ui2/tower_model.h)
        using namespace xr::ui2::tw;
        using xr::tower::Aircraft;
        check(baysFor("TWR", true).front() == "incoming" && baysFor("TWR", true).size() == 9 && baysFor("TWR", false).size() == 13 && baysFor("DEL", true).size() == 3,
              "tower: the bays of a position's board, the whole board when positions are not split");
        Aircraft dep; dep.kind = "dep"; dep.state = "clr_req"; dep.freq = "DEL";
        Aircraft ga = dep; ga.ga = true;
        Aircraft arr; arr.kind = "arr"; arr.state = "final"; arr.freq = "APP";
        Aircraft veh; veh.veh = true; veh.state = "idle"; veh.freq = "GND";
        Aircraft live; live.human = true; live.freq = "GND"; live.kind = "dep"; live.state = "taxi";
        check(autoBay(dep) == "pending" && autoBay(ga) == "taxi" && autoBay(arr) == "approach" && autoBay(live) == "live", "tower: the bay a state puts a strip in (TowerTrainer's rules)");
        arr.freq = "TWR";
        check(autoBay(arr) == "final", "tower: an arrival on Tower's frequency is in Final, on Approach's in Approach");
        check(onBoard(dep, "DEL", true) && !onBoard(dep, "TWR", true) && onBoard(dep, "TWR", false) && !onBoard(veh, "GND", true) && onBoard(live, "TWR", true) && !onBoard(live, "DEL", true),
              "tower: who is on a position's board (its frequency, live pilots but not at Delivery, no idle vehicles)");
        Aircraft hand = dep; hand.handoffTo = "TWR"; hand.handoffFrom = "GND"; hand.freq = "GND";
        check(incomingFor(hand, "TWR") && onBoard(hand, "TWR", true) && !incomingFor(hand, "GND"), "tower: a strip handed to TWR is on TWR's board, in Incoming");
        std::vector<Aircraft> ac(3);
        ac[0].cs = "FIN321"; ac[1].cs = "FIN1AB"; ac[2].cs = "SAS123";
        check(callsignMatches("fi", ac).size() == 2 && callsignMatches("123", ac).size() == 1 && callsignMatches("FIN321 run", ac).empty() && callsignMatches("", ac).empty(),
              "tower: callsign matches while the first word is typed, case-insensitive, also inside a callsign");
        check(completeCallsign("FI", ac) == "FIN" && completeCallsign("FIN3", ac) == "FIN321 " && completeCallsign("SA", ac) == "SAS123 " && completeCallsign("XX", ac) == "XX",
              "tower: Tab completes as far as the candidates agree, all the way when one is left");
        History h;
        std::string out;
        check(!h.up("x", &out), "tower: no history, nothing to recall");
        h.push("one"); h.push("two"); h.push("two"); h.push("   ");
        check(h.size() == 2 && h.up("draft", &out) && out == "two" && h.up("", &out) && out == "one" && !h.up("", &out) && h.down(&out) && out == "two" && h.down(&out) && out == "draft" && !h.down(&out),
              "tower: history up and down, the line being typed kept, no repeats, no blank lines");
        View v; v.cx = 100.f; v.cy = -50.f; v.ppm = 0.5f;
        const Vec o{10.f, 20.f}, size{800.f, 600.f};
        const Vec sp = toScreen(v, o, size, 300.f, 150.f), back = toWorld(v, o, size, sp.x, sp.y);
        check(std::fabs(back.x - 300.f) < 0.01f && std::fabs(back.y - 150.f) < 0.01f && sp.y < o.y + size.y * 0.5f, "tower radar: screen and world agree, north is up");
        const Vec mouse{600.f, 200.f}, under = toWorld(v, o, size, mouse.x, mouse.y);
        zoomAt(v, o, size, 2.f, mouse);
        const Vec still = toWorld(v, o, size, mouse.x, mouse.y);
        check(std::fabs(v.ppm - 1.f) < 1e-5f && std::fabs(still.x - under.x) < 0.01f && std::fabs(still.y - under.y) < 0.01f, "tower radar: zooming keeps the point under the mouse where it is");
        pan(v, 50.f, -20.f);
        const Vec moved = toWorld(v, o, size, mouse.x + 50.f, mouse.y - 20.f);
        check(std::fabs(moved.x - under.x) < 0.01f && std::fabs(moved.y - under.y) < 0.01f, "tower radar: dragging carries the picture with the mouse");
        check(ringStep(0.5f) == 200.f && ringStep(0.05f) == 2000.f && ringStep(0.001f) > 1852.f * 49 && rangeText(500) == "500 m" && rangeText(2000) == "2 km" && rangeText(1852 * 10) == "10 nm",
              "tower radar: round range rings about 70 px apart, labelled in m, km, nm");
        xr::tower::State st;
        View f = fit(st, size);
        check(f.ppm > 0.f && f.cx == 0.f, "tower radar: fitting an airport with nothing in it does not divide by zero");
        st.runways.push_back({"08/26", 45.f, {{"08", -1500.f, 0.f}, {"26", 1500.f, 0.f}}});
        f = fit(st, size);
        check(std::fabs(f.ppm - (800.f - 140.f) / 3000.f) < 1e-4f, "tower radar: fitting shows the whole runway, with room for labels at its ends");
        st.ac.resize(2);
        st.ac[0].x = 2400.f; st.ac[1].x = 9000.f;
        f = fit(st, size);
        check(std::fabs(f.ppm - (800.f - 140.f) / 3900.f) < 1e-4f, "tower radar: an aircraft just past the runway end is in the picture, one 7 km out is not");
        {   // the label filter: a busy apron stays readable
            Aircraft parked; parked.kind = "dep"; parked.state = "parked"; parked.cs = "FIN321";
            Aircraft quiet; quiet.veh = true; quiet.quiet = true; quiet.state = "roam";
            Aircraft taxi = parked; taxi.state = "taxi";
            Aircraft air = parked; air.state = "airborne"; air.onGround = false;
            check(labelLines(parked, false, false, false, 0.5f, true) == 2 && labelLines(parked, false, false, false, 0.2f, true) == 1 && labelLines(parked, false, false, false, 0.05f, true) == 0,
                  "tower radar: a parked aircraft's label shrinks to its callsign zoomed out, and goes further out still");
            Aircraft dormant = parked; dormant.dormant = true;
            check(labelLines(dormant, false, false, false, 0.5f, true) == 1 && labelLines(parked, true, false, false, 0.05f, true) == 2 && labelLines(parked, false, true, false, 0.05f, true) == 2 &&
                  labelLines(parked, false, false, true, 0.05f, true) == 2,
                  "tower radar: a dormant aircraft shows its callsign only; selected, under the mouse or calling it has its whole label");
            check(labelLines(quiet, false, false, false, 1.f, true) == 0 && labelLines(quiet, false, true, false, 1.f, true) == 2 && labelLines(taxi, false, false, false, 0.05f, false) == 1 &&
                  labelLines(taxi, false, false, false, 0.05f, true) == 2,
                  "tower radar: a service vehicle's label on hover only; with Labels off, the callsign alone");
            Aircraft sick = parked; sick.warn.push_back("EMG");
            check(labelLines(sick, false, false, false, 0.05f, true) == 2 && labelPriority(sick, false, false) > labelPriority(air, false, false) &&
                  labelPriority(air, false, false) > labelPriority(taxi, false, false) && labelPriority(taxi, false, false) > labelPriority(parked, false, false) &&
                  labelPriority(parked, false, false) > labelPriority(quiet, false, false) && labelPriority(quiet, true, false) == 5,
                  "tower radar: room goes to the selected aircraft, trouble, airborne, moving, parked, vehicles -- in that order");
            check(labelMayHide(parked, false, false) && labelMayHide(quiet, false, false) && !labelMayHide(parked, true, false) && !labelMayHide(parked, false, true) && !labelMayHide(taxi, false, false),
                  "tower radar: only parked aircraft and vehicles give their label up for want of room");
        }
        xr::tower::State s;
        s.endpoint = "127.0.0.1:8781";
        check(!bannerFor(s, "TOWER").show, "tower banner: none when Tower mode is off");
        s.phase = xr::tower::Phase::Unreachable; s.retryInS = 3.2f;
        Banner b = bannerFor(s, "TOWER");
        check(b.show && b.retry && b.level == 1 && b.title.find("server.py") != std::string::npos && b.title.find("127.0.0.1:8781") != std::string::npos && b.detail.find("4 s") != std::string::npos,
              "tower banner: unreachable -- start TowerTrainer, the address, when it tries again");
        xr::ui2::setLanguage("et");
        b = bannerFor(s, "TOWER");
        check(b.title.rfind("Käivita TowerTrainer (server.py)", 0) == 0 && b.title.find("127.0.0.1:8781") != std::string::npos && b.detail.find("4 s") != std::string::npos &&
              std::string(xr::ui2::tr("Steady green")) == "Püsiv roheline" && std::string(xr::ui2::tr(bayTitle("holding"))) == "Ootepunkt",
              "tower in Estonian: the banner with the address and seconds filled in, the light gun, the bays");
        xr::ui2::setLanguage("en");
        s.phase = xr::tower::Phase::HostAway;
        check(bannerFor(s, "TOWER").show && !bannerFor(s, "TOWER").retry, "tower banner: the trainer's window away, nothing to retry");
        check(std::string(signalMeaning("green steady", true)) == "cleared for takeoff" && std::string(signalMeaning("green steady", false)) == "cleared to land" && signals().size() == 5,
              "tower: the five light signals and what they mean on the ground and in the air");
        xr::tower::LogEntry e; e.cls = "pilot human";
        check(senderOf(e) == Sender::LivePilot && (e.cls = "ai", senderOf(e) == Sender::AiAtc) && (e.cls = "blk", senderOf(e) == Sender::Problem) && (e.cls = "sys", senderOf(e) == Sender::System),
              "tower: whose line a log entry is, from the trainer's class");
        xr::tower::LiveCall c; c.text = "runway 26 cleared for takeoff";
        std::vector<xr::tower::LogEntry> lg(1); lg[0].text = "Finnair 321, runway 26 cleared for takeoff, wind calm";
        check(inLog(c, lg) && (c.text = "line up", !inLog(c, lg)), "tower: a live call disappears once the trainer's log has it");
    }

    printf(fails ? "%d FAILED\n" : "ui_logic_test OK\n", fails);
    return fails ? 1 : 0;
}
