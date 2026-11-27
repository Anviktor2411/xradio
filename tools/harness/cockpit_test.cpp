// 0.7: the plain logic behind the shared cockpit, the quick-tune slots and the
// connection's round trip (plugin/src/cockpit.h) -- no X-Plane, no sockets.
#include "cockpit.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace xr;
using namespace xr::cockpit;

static int failures = 0;

static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

static CrewPanel panel(uint32_t c1, uint32_t c2, uint32_t s1, uint32_t s2, uint8_t tx, uint8_t rx) {
    CrewPanel p{};
    p.com1Khz = c1; p.com2Khz = c2; p.com1StbyKhz = s1; p.com2StbyKhz = s2; p.txSel = tx; p.rxMask = rx;
    p.flags = CP_COM1_POWER | CP_COM2_POWER;
    p.com1Vol = 0.8f; p.com2Vol = 0.5f;
    return p;
}

int main() {
    printf("\nthe radio panel\n");
    {
        check("the air band, 8.33 kHz channels included", validComKhz(118000) && validComKhz(136990) &&
              validComKhz(121505) && !validComKhz(117975) && !validComKhz(137000) && !validComKhz(0));
        CrewPanel wild = panel(500, 121500, 999999, 118100, 9, 0x7F);
        wild.com1Vol = NAN; wild.com2Vol = 4.f;
        const CrewPanel s = sanitized(wild);
        check("a panel from the network: frequencies out of the band are left alone",
              s.com1Khz == 0 && s.com2Khz == 121500 && s.com1StbyKhz == 0 && s.com2StbyKhz == 118100);
        check("and so is a transmit selector that is neither radio", s.txSel == 0);
        check("receive buttons keep only the two radios", s.rxMask == (RX_COM1 | RX_COM2));
        check("a volume that is not a number is no volume, one past full is full", s.com1Vol < 0.f && s.com2Vol == 1.f);

        const CrewPanel before = panel(118100, 121500, 119700, 122800, TX_COM1, RX_COM1);
        CrewPanel now = before;
        check("nothing turned, nothing asked", !asksAnything(changes(before, now)));
        now.com1Khz = 127905; now.txSel = TX_COM2; now.com1Vol = 0.1f;
        const CrewPanel t = changes(before, now);
        check("a knob turned is asked for, the rest left alone",
              t.com1Khz == 127905 && t.txSel == TX_COM2 && t.com2Khz == 0 && t.com1StbyKhz == 0 && t.rxMask == 0xFF);
        check("but never the volume: each headset is its own", t.com1Vol < 0.f && t.com2Vol < 0.f);
        const CrewPanel after = applied(before, t);
        check("a tune applied changes what it asks for, and only that",
              after.com1Khz == 127905 && after.txSel == TX_COM2 && after.com2Khz == 121500 && after.com1StbyKhz == 119700);
        check("the panel shows a tune once it is through", shows(after, t) && !shows(before, t));
        check("a tune of nothing is shown by any panel", shows(before, nothing()));
        check("the same tuning, whatever the knobs", sameTuning(before, [&] { CrewPanel x = before; x.com2Vol = 0.f; return x; }()));
        check("but not the same panel", !samePanel(before, [&] { CrewPanel x = before; x.com2Vol = 0.f; return x; }()));
    }

    printf("\nwords\n");
    {
        check("roles have names", std::string(roleName(CREW_ROLE_COPILOT)) == "copilot" &&
              std::string(roleName(CREW_ROLE_OBSERVER)) == "observer" && std::string(roleName(CREW_ROLE_PILOT)) == "pilot");
        check("the setting picks the role, copilot unless observer",
              roleFromSetting("observer") == CREW_ROLE_OBSERVER && roleFromSetting("copilot") == CREW_ROLE_COPILOT &&
              roleFromSetting("nonsense") == CREW_ROLE_COPILOT);
        check("a refusal says why, naming the pilot", refusalText(CREW_BAD_CODE, "ESNA12").find("ESNA12") != std::string::npos &&
              refusalText(CREW_NOT_OPEN, "ESNA12").find("not opened") != std::string::npos);
        check("and every reason has words", !refusalText(CREW_FULL, "X").empty() && !refusalText(CREW_CLOSED, "X").empty() &&
              !refusalText(CREW_BUSY, "").empty() && !refusalText(CREW_NO_SUCH, "").empty() && !refusalText(99, "").empty());
    }

    printf("\nquick-tune slots\n");
    {
        check("frequencies typed three ways", parseKhz("118.100") == 118100 && parseKhz("118100") == 118100 &&
              parseKhz("118.1") == 118100 && parseKhz(" 121.5 ") == 121500);
        check("and nonsense is nothing", parseKhz("") == 0 && parseKhz("abc") == 0 && parseKhz("108.000") == 0 &&
              parseKhz("118.1x") == 0);
        const auto list = parsePresets("TWR=118.100;gnd = 121.800; 123.450;BAD=999.9;ATIS=127.905");
        check("labels and frequencies, a bare frequency labelled by itself, nonsense skipped",
              list.size() == 4 && list[0].label == "TWR" && list[0].khz == 118100 && list[1].label == "GND" &&
              list[2].label == "123.450" && list[2].khz == 123450 && list[3].label == "ATIS",
              std::to_string(list.size()));
        check("eight at most", parsePresets("1=118.000;2=118.100;3=118.200;4=118.300;5=118.400;6=118.500;7=118.600;8=118.700;9=118.800").size() == 8);
        check("and they come back as they were written", formatPresets(list) == "TWR=118.100;GND=121.800;123.450;ATIS=127.905",
              formatPresets(list));
        check("long labels are cut to fit the button", parsePresets("APPROACHCONTROL=119.700")[0].label == "APPROACH");
        check("the frequency as the panel shows it", khzText(121505) == "121.505" && khzText(118000) == "118.000");
    }

    printf("\nthe round trip\n");
    {
        RttMeter m;
        check("nothing measured to begin with", !m.haveRtt() && m.lossPct(0.0) == 0.0);
        m.sent(1, 10.0);
        check("an answer to the ping that was sent counts", m.answered(1, 10.040) && m.haveRtt() && std::fabs(m.rttMs() - 40.0) < 0.5,
              std::to_string(m.rttMs()));
        check("the same answer twice does not", !m.answered(1, 10.050));
        check("nor an answer to a ping never sent", !m.answered(7, 10.050));
        m.sent(2, 12.0);
        m.answered(2, 12.120);
        check("the round trip is smoothed, not jumping to the last one", m.rttMs() > 40.0 && m.rttMs() < 120.0, std::to_string(m.rttMs()));
        check("and the jitter follows the difference", m.jitterMs() > 5.0, std::to_string(m.jitterMs()));
        m.sent(3, 14.0);
        m.sent(4, 16.0);
        m.answered(4, 16.030);
        check("a ping not answered within three seconds is lost: one of four", std::fabs(m.lossPct(30.0) - 25.0) < 0.01,
              std::to_string(m.lossPct(30.0)));
        check("and an answer that late does not count", !m.answered(3, 18.0));
        RttMeter old;                                   // a server from before 0.7: empty pongs
        old.sent(1, 1.0);
        check("an empty pong is matched to the one ping waiting", old.answeredAnonymous(1.050) && std::fabs(old.rttMs() - 50.0) < 0.5);
        old.sent(2, 2.0);
        old.sent(3, 2.1);
        check("but not when two are waiting: it could be either", !old.answeredAnonymous(2.2));
        check("voice lost as a share of what came", std::fabs(voiceLossPct(90, 10) - 10.0) < 1e-9 && voiceLossPct(0, 0) == 0.0);
    }

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all cockpit tests passed\n");
    return 0;
}
