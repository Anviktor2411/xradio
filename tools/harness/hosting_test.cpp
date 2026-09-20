// Hosting, end to end: the plugin runs the server, its own client joins it,
// and a real peer connects over real UDP -- no Python, no VPS, nothing to
// configure. This is the whole point of the feature, so it is tested the way
// a pilot would experience it: switch it on in the settings window, then see
// whether someone else can actually get in.
#include "harness.h"
#include "protocol.h"
#include "settings.h"
#include "ui.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define CLOSESOCK closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define CLOSESOCK ::close
#endif

extern "C" {
int  XPluginStart(char*, char*, char*);
int  XPluginEnable(void);
void XPluginDisable(void);
void XPluginStop(void);
}

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

static const char* kCfgPath = "/tmp/xradio-harness/xradio.cfg";
static constexpr int kWin = 2;              // the settings window
static uint16_t      g_port = 49610;

static std::vector<std::string> drawSettings() { return harness::drawWindow(kWin); }
static bool shows(const std::vector<std::string>& v, const std::string& s) {
    return harness::drawnContains(v, s);
}
static void dump(const std::vector<std::string>& v) {
    for (const auto& l : v) printf("        %s\n", l.c_str());
}

// Run the sim for a while: the flight loop sends our position, the window
// pumps, and the hosted server gets time to relay.
static void fly(double seconds, double lat = 57.85, double lon = 27.02) {
    const int steps = (int)(seconds / 0.05);
    for (int i = 0; i < steps; ++i) {
        harness::set("sim/flightmodel/position/latitude", lat);
        harness::set("sim/flightmodel/position/longitude", lon);
        harness::tick(0.05f);
        harness::draw();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ---------------------------------------------------------------------------
// A second pilot, speaking the wire protocol directly.
// ---------------------------------------------------------------------------
class Peer {
public:
    Peer(const char* callsign, double lat, double lon)
        : callsign_(callsign), lat_(lat), lon_(lon) {
        fd_ = (int)socket(AF_INET, SOCK_DGRAM, 0);
        timeval tv{0, 200 * 1000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        memset(&dst_, 0, sizeof(dst_));
        dst_.sin_family = AF_INET;
        dst_.sin_port = htons(g_port);
        dst_.sin_addr.s_addr = inet_addr("127.0.0.1");
    }
    ~Peer() { if (fd_ >= 0) CLOSESOCK(fd_); }

    bool login(const char* password = "", const char* livery = "Ryanair",
               bool weatherSource = false) {
        xr::LoginPayload lp{};
        snprintf(lp.callsign, sizeof(lp.callsign), "%s", callsign_.c_str());
        snprintf(lp.acIcao, sizeof(lp.acIcao), "%s", "B738");
        snprintf(lp.livery, sizeof(lp.livery), "%s", livery);
        snprintf(lp.password, sizeof(lp.password), "%s", password);
        lp.protoVer = xr::kProtoVersion;
        lp.flags = weatherSource ? xr::LF_WEATHER_SOURCE : 0;
        rejected_ = 0;
        send(xr::PT_LOGIN, 0, &lp, sizeof(lp));

        uint8_t buf[xr::kMaxPacket];
        for (int i = 0; i < 25; ++i) {
            const int n = recvAny(buf, sizeof(buf));
            if (n >= (int)(sizeof(xr::Header) + 4)) {
                xr::Header h{};
                memcpy(&h, buf, sizeof(h));
                if (h.type == xr::PT_LOGIN_ACK && n >= (int)(sizeof(h) + sizeof(xr::LoginAckPayload))) {
                    xr::LoginAckPayload ack{};
                    memcpy(&ack, buf + sizeof(h), sizeof(ack));
                    sid_ = ack.sessionId;
                    return sid_ != 0;
                }
                if (h.type == xr::PT_LOGIN_REJECT) {
                    xr::LoginRejectPayload rj{};
                    memcpy(&rj, buf + sizeof(h), sizeof(rj));
                    rejected_ = rj.reason;
                    return false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        return false;
    }
    uint16_t rejectedWith() const { return rejected_; }

    void position(uint32_t com1 = 122800) {
        xr::PositionPayload p{};
        p.lat = lat_; p.lon = lon_;
        p.altMslM = 914.f; p.headingTrue = 270.f; p.trackTrue = 270.f;
        p.gsMs = 60.f;
        p.com1Khz = com1;
        p.rxMask = xr::RX_COM1;
        p.txRadio = xr::TX_COM1;
        p.timeMs = ++clock_;
        send(xr::PT_POSITION, sid_, &p, sizeof(p));
    }

    void text(const char* body, uint32_t freq = 122800) {
        uint8_t buf[xr::kMaxPacket];
        xr::TextHeader th{};
        th.freqKhz = freq;
        th.fromSession = sid_;
        snprintf(th.from, sizeof(th.from), "%s", callsign_.c_str());
        const uint16_t n = (uint16_t)strlen(body);
        th.textLen = n;
        memcpy(buf, &th, sizeof(th));
        memcpy(buf + sizeof(th), body, n);
        send(xr::PT_TEXT, sid_, buf, (int)sizeof(th) + n);
    }

    // Everything received in `ms`, split by packet type.
    struct Bag {
        int traffic = 0;
        std::vector<std::string> sawCallsigns;
        std::vector<std::string> sawTypes;      // "B738/Ryanair" per entry
        std::vector<std::string> texts;
        std::vector<xr::WeatherPayload> weather;
    };
    Bag drain(int ms) {
        Bag bag;
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(ms);
        uint8_t buf[xr::kMaxPacket];
        while (std::chrono::steady_clock::now() < end) {
            const int n = recvAny(buf, sizeof(buf));
            if (n < (int)sizeof(xr::Header)) continue;
            xr::Header h{};
            memcpy(&h, buf, sizeof(h));
            if (h.type == xr::PT_TRAFFIC) {
                ++bag.traffic;
                xr::TrafficHeader th{};
                memcpy(&th, buf + sizeof(h), sizeof(th));
                size_t off = sizeof(h) + sizeof(th);
                for (int i = 0; i < (int)th.count; ++i) {
                    if (off + sizeof(xr::TrafficEntry) > (size_t)n) break;
                    xr::TrafficEntry e{};
                    memcpy(&e, buf + off, sizeof(e));
                    off += sizeof(e);
                    char cs[17] = {0};
                    memcpy(cs, e.callsign, 16);
                    bag.sawCallsigns.push_back(cs);
                    char ty[9] = {0}, lv[17] = {0};
                    memcpy(ty, e.acIcao, 8);
                    memcpy(lv, e.livery, 16);
                    bag.sawTypes.push_back(std::string(ty) + "/" + lv);
                }
            } else if (h.type == xr::PT_WEATHER) {
                if (n >= (int)(sizeof(h) + sizeof(xr::WeatherPayload))) {
                    xr::WeatherPayload w{};
                    memcpy(&w, buf + sizeof(h), sizeof(w));
                    bag.weather.push_back(w);
                }
            } else if (h.type == xr::PT_TEXT) {
                xr::TextHeader th{};
                memcpy(&th, buf + sizeof(h), sizeof(th));
                const size_t off = sizeof(h) + sizeof(th);
                const int len = (int)th.textLen;
                if (off + (size_t)len <= (size_t)n && len > 0) {
                    bag.texts.push_back(std::string((const char*)buf + off, (size_t)len));
                }
            }
        }
        return bag;
    }

    void weather(const xr::WeatherPayload& w) {
        send(xr::PT_WEATHER, sid_, &w, (int)sizeof(w));
    }

    uint32_t sid() const { return sid_; }

private:
    void send(uint8_t type, uint32_t sid, const void* payload, int len) {
        uint8_t buf[xr::kMaxPacket];
        xr::Header h{};
        h.magic = xr::kMagic;
        h.type = type;
        h.version = (uint8_t)xr::kProtoVersion;
        h.payloadLen = (uint16_t)len;
        h.sessionId = sid;
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), payload, (size_t)len);
        sendto(fd_, (const char*)buf, sizeof(h) + (size_t)len, 0,
               (const sockaddr*)&dst_, sizeof(dst_));
    }
    int recvAny(uint8_t* buf, size_t max) {
        return (int)recv(fd_, (char*)buf, max, 0);
    }

    std::string callsign_;
    uint16_t    rejected_ = 0;
    double      lat_, lon_;
    int         fd_ = -1;
    uint32_t    sid_ = 0;
    uint32_t    clock_ = 0;
    sockaddr_in dst_{};
};

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

// Switch hosting on through the window, exactly as a pilot would.
static void turnHostingOn() {
    harness::menu(1);                       // Plugins > XRadio > Settings...
    drawSettings();
    int tx = 0, ty = 0, top = 0, left = 0;
    harness::windowTop(kWin, &top, &left);
    harness::drawnAt("Hosting", &tx, &ty);
    harness::click(kWin, tx + 10, ty);      // the Hosting tab
    drawSettings();

    int lx = 0, ly = 0;
    harness::drawnAt("Host a flight here", &lx, &ly);
    harness::click(kWin, left + xr::ui::Ctx::kValueX + 10, ly);
    drawSettings();

    int bx = 0, by = 0;
    harness::drawnAt("Save & apply", &bx, &by);
    harness::click(kWin, bx + 20, by);
    drawSettings();
}

int main(int argc, char** argv) {
    if (argc > 1) g_port = (uint16_t)atoi(argv[1]);
    if (system("mkdir -p /tmp/xradio-harness") != 0) return 1;

    // Start from a config that hosts on our test port but is switched off,
    // so the test can turn it on through the window and watch what happens.
    {
        xr::Settings s;
        s.callsign = "HOSTER";
        s.acIcao = "";                     // from the sim
        s.host = "127.0.0.1";
        s.port = "1";                      // deliberately useless
        s.hostPort = std::to_string(g_port);
        s.hostEnabled = false;
        s.hostUpnp = false;                // no router in CI, and no waiting
        s.autoConnect = true;
        xr::saveSettings(s, kCfgPath);
    }

    harness::set("sim/cockpit2/radios/actuators/com1_frequency_hz_833", 122800);
    harness::set("sim/cockpit2/radios/actuators/audio_selection_com1", 1);
    // A powered-up aircraft: the radios need the avionics bus, their own
    // switches and volts behind them, exactly as in the sim.
    harness::set("sim/cockpit2/switches/avionics_power_on", 1);
    harness::set("sim/cockpit2/radios/actuators/com1_power", 1);
    harness::set("sim/cockpit2/radios/actuators/com2_power", 1);
    harness::set("sim/cockpit2/electrical/bus_volts", 24.0);
    harness::set("sim/cockpit2/radios/actuators/audio_com_selection", 6);
    harness::set("sim/flightmodel/position/elevation", 914.0);
    // What the sim says we are flying. The config leaves the type blank, so
    // this is what everyone else must see.
    harness::setString("sim/aircraft/view/acf_ICAO", "A20N");
    harness::setString("sim/aircraft/view/acf_livery_path",
                       "Aircraft/ToLiss/A320/liveries/Lufthansa/");

    harness::resetWindows();
    char n[256], sg[256], d[256];
    XPluginStart(n, sg, d);
    XPluginEnable();

    printf("\nbefore hosting\n");
    {
        fly(0.3);
        auto w = harness::draw();
        check("not hosting yet", !shows(w, "Hosting  \xc2\xb7"));
        check("and not connected to anything either",
              !shows(w, "connected to") || shows(w, "socket error"));
    }

    printf("\nswitching hosting on in the window\n");
    {
        turnHostingOn();
        check("the window closed, so it saved", !harness::windowVisible(kWin));

        xr::Settings saved;
        xr::loadSettings(saved, kCfgPath);
        check("hosting was written to the config", saved.hostEnabled,
              "hosting=" + std::string(saved.hostEnabled ? "yes" : "no"));

        fly(1.2);
        auto w = harness::draw();
        check("the main window says it is hosting", shows(w, "Hosting"));
        check("our own client connected to it", shows(w, "connected to 127.0.0.1"));
        if (failures) dump(w);
    }

    printf("\na second pilot joins\n");
    Peer peer("FRIEND", 57.855, 27.025);
    {
        check("the peer logs in to the hosted server", peer.login(),
              "sid=" + std::to_string(peer.sid()));

        for (int i = 0; i < 12; ++i) {
            peer.position();
            fly(0.1, 57.85, 27.02);
        }
        auto w = harness::draw();
        check("the host sees the peer in its traffic list", shows(w, "FRIEND"));
        if (!shows(w, "FRIEND")) dump(w);

        auto bag = peer.drain(500);
        check("the peer receives traffic from the hosted server", bag.traffic > 0,
              std::to_string(bag.traffic) + " packets");
        check("and sees the host's aircraft in it",
              contains(bag.sawCallsigns, "HOSTER"),
              bag.sawCallsigns.empty() ? "nothing" : bag.sawCallsigns[0]);
        check("as the type and livery the sim reported, not a typed-in default",
              contains(bag.sawTypes, "A20N/Lufthansa"),
              bag.sawTypes.empty() ? "nothing" : bag.sawTypes[0]);
        auto w2 = harness::draw();
        check("the host sees the peer's type too", shows(w2, "B738"));
    }

    printf("\nchanging aircraft mid-flight\n");
    {
        harness::setString("sim/aircraft/view/acf_ICAO", "B738");
        harness::setString("sim/aircraft/view/acf_livery_path", "Aircraft/Zibo/liveries/Delta/");
        // The plugin checks every 3 s and logs in again; give it time to
        // reconnect and the peer time to get a traffic packet with the new type.
        bool seen = false;
        for (int i = 0; i < 12 && !seen; ++i) {
            for (int j = 0; j < 5; ++j) { peer.position(); fly(0.1); }
            auto bag = peer.drain(200);
            seen = contains(bag.sawTypes, "B738/Delta");
        }
        check("everyone sees the new aircraft within a few seconds", seen);
    }

    printf("\nthe radio works through the hosted server\n");
    {
        peer.position();
        fly(0.2);
        peer.text("FRIEND checking in");
        for (int i = 0; i < 8; ++i) { peer.position(); fly(0.1); }
        auto w = harness::draw();
        check("the host hears the peer on the radio", shows(w, "checking in"));
        if (!shows(w, "checking in")) dump(w);

        harness::menu(4);                       // "Send test message"
        for (int i = 0; i < 6; ++i) { peer.position(); fly(0.1); }
        auto bag = peer.drain(400);
        check("and the peer hears the host", !bag.texts.empty(),
              bag.texts.empty() ? "nothing" : bag.texts[0]);

        // Typed into the main window: click the Say row, type, Enter.
        auto w0 = harness::draw();
        int sx = 0, sy = 0;
        check("the main window has a place to type", harness::drawnAt("Say:", &sx, &sy));
        harness::click(1, sx + 40, sy);
        harness::typeText(1, "Hoster here, taxiing to 26");
        auto typing = harness::draw();
        check("what is typed shows in the window", shows(typing, "> Hoster here, taxiing to 26"));
        harness::pressVk(1, 0x0D);
        for (int i = 0; i < 6; ++i) { peer.position(); fly(0.1); }
        auto typedBag = peer.drain(400);
        bool got = false;
        for (const auto& t : typedBag.texts) if (t == "Hoster here, taxiing to 26") got = true;
        check("Enter sends it to the other pilot", got,
              typedBag.texts.empty() ? "nothing" : typedBag.texts.back());
        auto after = harness::draw();
        check("the field clears and the line appears in our own log",
              shows(after, "HOSTER: Hoster here, taxiing to 26") && !shows(after, "> Hoster"));
        // A click elsewhere in the window hands the keyboard back to the sim.
        harness::click(1, sx + 40, sy + 200);
        harness::typeText(1, "zzz");
        check("clicking away stops capturing keys", !shows(harness::draw(), "zzz"));
    }

    printf("\nthe hosting tab reports what is going on\n");
    {
        harness::menu(1);
        drawSettings();
        int tx = 0, ty = 0;
        harness::drawnAt("Hosting", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        for (int i = 0; i < 6; ++i) { peer.position(); fly(0.1); }
        auto v = drawSettings();
        check("it says the server is running", shows(v, "Running on port"));
        check("it shows an address to give out", shows(v, "Friends type:"));
        check("it names who is connected", shows(v, "HOSTER") && shows(v, "FRIEND"));
        check("it does not claim the router opened anything",
              !shows(v, "Router opened"));
        if (failures) dump(v);
    }

    printf("\na cold and dark aircraft is off the air\n");
    {
        // A radio needs electricity. Nothing about this was true before:
        // an aircraft parked with the master off was still transmitting.
        Peer awake("PWR001", 57.858, 27.028);
        check("another pilot is listening", awake.login());
        for (int i = 0; i < 4; ++i) { awake.position(); fly(0.15); }

        harness::set("sim/cockpit2/switches/avionics_power_on", 0);
        harness::set("sim/cockpit2/electrical/bus_volts", 0.0);
        fly(0.6);

        auto w = harness::draw();
        check("the window says the radios have no power", shows(w, "no power"));
        if (!shows(w, "no power")) dump(w);

        harness::ptt(true);
        fly(0.5);
        auto w2 = harness::draw();
        check("keying does nothing at all", !shows(w2, "** TX **"));
        harness::ptt(false);

        awake.drain(200);
        awake.text("anyone on frequency");
        fly(0.8);
        auto w3 = harness::draw();
        check("and nothing is heard either",
              !shows(w3, "anyone on frequency"));

        // Master and avionics back on: the radio comes alive again.
        harness::set("sim/cockpit2/switches/avionics_power_on", 1);
        harness::set("sim/cockpit2/electrical/bus_volts", 24.0);
        fly(0.6);
        auto w4 = harness::draw();
        check("power restored, the radios are back", !shows(w4, "no power"));
        harness::ptt(true);
        fly(0.4);
        check("and keying works again", shows(harness::draw(), "** TX **"));
        harness::ptt(false);
        awake.text("reading you now");
        fly(0.8);
        check("so does hearing", shows(harness::draw(), "reading you now"));
    }

    printf("\nthe window is narrowed\n");
    {
        // X-Plane lets a pilot drag this window down to 320 px. Text drawn
        // past the edge lands on the cockpit, which looks like a crash.
        int l = 0, t = 0, r = 0, b = 0;
        harness::windowRect(1, &l, &t, &r, &b);
        harness::setWindowRect(1, l, t, l + 320, b);
        auto w = harness::draw();
        int over = 0;
        for (const auto& d : harness::drawnPositions()) {
            // 7 px per character in the harness's font
            const int endX = d.x + 7 * (int)d.text.size();
            if (endX > l + 320) ++over;
        }
        if (over) {
            for (const auto& d : harness::drawnPositions()) {
                const int endX = d.x + 7 * (int)d.text.size();
                if (endX > l + 320)
                    printf("        overflow by %d px: %s\n", endX - (l + 320), d.text.c_str());
            }
        }
        check("nothing is drawn past the right edge", over == 0,
              std::to_string(over) + " lines overflow");
        check("and the window still shows its content", shows(w, "XRadio"));
        harness::setWindowRect(1, l, t, r, b);
    }

    printf("\nthe host's sky reaches everyone else\n");
    {
        // The whole point: one pilot breaking out of cloud at 600 ft while
        // another is in sunshine is worse than no weather sharing at all.
        // Set a distinctive sky on the host's sim and see it arrive intact.
        harness::set("sim/time/zulu_time_sec", 43200.f);        // 12:00Z
        harness::set("sim/time/local_date_days", 180);
        harness::set("sim/weather/region/sealevel_pressure_pas", 99400.f);
        harness::set("sim/weather/region/sealevel_temperature_c", 7.f);
        harness::set("sim/weather/region/visibility_reported_sm", 4.5f);
        harness::set("sim/weather/region/rain_percent", 0.6f);
        harness::setArray("sim/weather/region/cloud_base_msl_m", {180.f, 2400.f, 0.f});
        harness::setArray("sim/weather/region/cloud_coverage_percent", {0.9f, 0.4f, 0.f});
        harness::setArray("sim/weather/region/wind_speed_msc",
                          {8.f, 12.f, 18.f, 25.f, 31.f, 38.f, 44.f, 50.f, 55.f, 58.f, 60.f, 52.f, 40.f});
        harness::setArray("sim/weather/region/wind_direction_degt",
                          {210.f, 215.f, 220.f, 228.f, 235.f, 240.f, 245.f, 250.f, 255.f, 258.f, 260.f, 262.f, 265.f});

        Peer sky("SKY001", 57.856, 27.026);
        check("a pilot joins the hosted flight", sky.login());
        // The host sends weather on a 10 s clock; it also sends one on the
        // first tick after login, so a short wait is enough.
        Peer::Bag bag;
        for (int i = 0; i < 24 && bag.weather.empty(); ++i) {
            fly(0.4);
            auto got = sky.drain(200);
            if (!got.weather.empty()) bag = got;
        }
        check("the host's weather arrives", !bag.weather.empty());
        if (!bag.weather.empty()) {
            const xr::WeatherPayload& w = bag.weather.front();
            check("at the right time of day", std::fabs(w.zuluTimeSec - 43200.f) < 1.f,
                  std::to_string(w.zuluTimeSec));
            check("on the right date", w.dateDays == 180, std::to_string(w.dateDays));
            check("with the host's pressure",
                  std::fabs(w.seaLevelPressurePa - 99400.f) < 1.f,
                  std::to_string(w.seaLevelPressurePa));
            check("and visibility", std::fabs(w.visibilitySm - 4.5f) < 0.01f);
            check("the overcast layer's base comes through",
                  std::fabs(w.cloudBaseM[0] - 180.f) < 0.5f &&
                  std::fabs(w.cloudCoverage[0] - 0.9f) < 0.01f);
            check("the second cloud layer too",
                  std::fabs(w.cloudBaseM[1] - 2400.f) < 0.5f);
            check("all thirteen wind layers, not just the surface",
                  std::fabs(w.windSpeedMs[0] - 8.f) < 0.01f &&
                  std::fabs(w.windSpeedMs[6] - 44.f) < 0.01f &&
                  std::fabs(w.windSpeedMs[12] - 40.f) < 0.01f,
                  std::to_string(w.windSpeedMs[12]));
            check("winds aloft keep their direction",
                  std::fabs(w.windDirDeg[12] - 265.f) < 0.01f);
        }
    }

    printf("\nand a sky shared by someone else is flown in\n");
    {
        // The other direction, as a pilot who is not the authority: untick
        // "share", so the claim passes to whoever does want it, and the
        // weather they send has to land in this sim's own datarefs.
        harness::menu(1);
        drawSettings();
        int tx = 0, ty = 0, top = 0, left = 0;
        harness::windowTop(kWin, &top, &left);
        harness::drawnAt("Hosting", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();
        int lx = 0, ly = 0;
        check("there is a share toggle", harness::drawnAt("Share my weather", &lx, &ly));
        harness::click(kWin, left + xr::ui::Ctx::kValueX + 10, ly);
        drawSettings();
        int bx = 0, by = 0;
        harness::drawnAt("Save & apply", &bx, &by);
        harness::click(kWin, bx + 20, by);
        drawSettings();
        fly(1.5);                       // reconnects without the claim
        {
            xr::Settings saved;
            xr::loadSettings(saved, kCfgPath);
            check("sharing is off in the saved settings", !saved.shareWeather);
            check("following is still on", saved.followWeather);
        }

        harness::set("sim/weather/region/sealevel_pressure_pas", 101325.f);
        harness::set("sim/weather/region/visibility_reported_sm", 10.f);
        harness::set("sim/time/zulu_time_sec", 3600.f);           // 01:00Z

        xr::WeatherPayload w{};
        w.zuluTimeSec = 75600.f;                                  // 21:00Z
        w.dateDays = 200;
        w.seaLevelPressurePa = 98700.f;
        w.visibilitySm = 1.25f;
        w.seaLevelTempC = -3.f;
        w.cloudBaseM[0] = 120.f;
        w.cloudCoverage[0] = 1.f;
        for (int i = 0; i < xr::kAirLayers; ++i) {
            w.windSpeedMs[i] = 5.f + (float)i;
            w.windDirDeg[i]  = 90.f + (float)i;
            w.turbulence[i]  = 0.2f;
        }

        Peer boss("SKY002", 57.857, 27.027);
        check("the pilot sharing their sky joins", boss.login("", "Ryanair", true));
        boss.weather(w);
        fly(1.0);

        check("the clock moved to their time",
              std::fabs(harness::get("sim/time/zulu_time_sec") - 75600.0) < 1.0,
              std::to_string(harness::get("sim/time/zulu_time_sec")));
        check("the pressure followed",
              std::fabs(harness::get("sim/weather/region/sealevel_pressure_pas") - 98700.0) < 1.0,
              std::to_string(harness::get("sim/weather/region/sealevel_pressure_pas")));
        check("so did the visibility",
              std::fabs(harness::get("sim/weather/region/visibility_reported_sm") - 1.25) < 0.01);
        const auto& winds = harness::getArray("sim/weather/region/wind_speed_msc");
        check("every wind layer was written",
              winds.size() == (size_t)xr::kAirLayers && std::fabs(winds[12] - 17.f) < 0.01f,
              winds.empty() ? "none" : std::to_string(winds.size()) + " layers");
        check("X-Plane was told to apply it now, not in a minute",
              harness::get("sim/weather/region/update_immediately") == 1.0);

        // Small differences are left alone. Writing zulu time on every
        // packet would fight the sim's own clock, which a pilot sees as the
        // sun twitching; only a real drift is worth a correction.
        harness::set("sim/time/zulu_time_sec", 75600.f);
        w.zuluTimeSec = 75605.f;                 // five seconds out
        boss.weather(w);
        fly(0.8);
        check("a five-second difference is not worth snapping the clock",
              std::fabs(harness::get("sim/time/zulu_time_sec") - 75600.0) < 0.5,
              std::to_string(harness::get("sim/time/zulu_time_sec")));

        w.zuluTimeSec = 79200.f;                 // an hour out
        boss.weather(w);
        fly(0.8);
        check("an hour out is",
              std::fabs(harness::get("sim/time/zulu_time_sec") - 79200.0) < 1.0,
              std::to_string(harness::get("sim/time/zulu_time_sec")));

        // A pilot who wants their own weather keeps it.
        harness::menu(1);
        drawSettings();
        harness::drawnAt("Traffic", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();
        check("there is a follow toggle", harness::drawnAt("Follow the host", &lx, &ly));
        harness::click(kWin, left + xr::ui::Ctx::kValueX + 10, ly);
        drawSettings();
        harness::drawnAt("Save & apply", &bx, &by);
        harness::click(kWin, bx + 20, by);
        drawSettings();
        fly(1.5);

        harness::set("sim/weather/region/sealevel_pressure_pas", 101325.f);
        w.seaLevelPressurePa = 95000.f;
        boss.login("", "Ryanair", true);
        boss.weather(w);
        fly(1.0);
        check("with following off, our own weather is left alone",
              std::fabs(harness::get("sim/weather/region/sealevel_pressure_pas") - 101325.0) < 1.0,
              std::to_string(harness::get("sim/weather/region/sealevel_pressure_pas")));
    }

    printf("\nchanging the callsign leaves no ghost behind\n");
    {
        // Saving a new callsign reconnects, which the server sees as a second
        // pilot unless the old session says goodbye. It used to not say it,
        // so the pilot watched their own previous callsign sitting in their
        // traffic list at 0.0 nm until the server's 15 s timeout ran out.
        harness::menu(1);
        drawSettings();
        int tx = 0, ty = 0, top = 0, left = 0;
        harness::windowTop(kWin, &top, &left);
        harness::drawnAt("Connection", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();

        int lx = 0, ly = 0;
        check("the callsign field is there", harness::drawnAt("Callsign", &lx, &ly));
        harness::click(kWin, left + xr::ui::Ctx::kValueX + 10, ly);
        drawSettings();
        for (int i = 0; i < 7; ++i) harness::pressVk(kWin, 0x08);   // backspace HOSTER
        harness::typeText(kWin, "SH6767");
        drawSettings();

        int bx = 0, by = 0;
        harness::drawnAt("Save & apply", &bx, &by);
        harness::click(kWin, bx + 20, by);
        drawSettings();
        fly(2.0);

        auto w = harness::draw();
        check("we are back on under the new callsign", shows(w, "SH6767"));

        // Only the traffic rows matter: chat the old callsign sent earlier
        // stays in the radio log, and should.
        bool ghost = false;
        for (const auto& line : w)
            if (line.find(" ft ") != std::string::npos &&
                line.find("HOSTER") != std::string::npos) ghost = true;
        check("the old callsign is not in our own traffic list", !ghost);
        if (ghost) dump(w);
    }

    printf("\na flight password\n");
    {
        // Set one through the window; the hosted server restarts requiring it
        // and our own client logs back in with it.
        harness::menu(1);
        drawSettings();
        int tx = 0, ty = 0, top = 0, left = 0;
        harness::windowTop(kWin, &top, &left);
        harness::drawnAt("Connection", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();
        int lx = 0, ly = 0;
        check("there is a password field", harness::drawnAt("Flight password", &lx, &ly));
        harness::click(kWin, left + xr::ui::Ctx::kValueX + 10, ly);
        drawSettings();
        harness::typeText(kWin, "cumulus");
        drawSettings();
        int bx = 0, by = 0;
        harness::drawnAt("Save & apply", &bx, &by);
        harness::click(kWin, bx + 20, by);
        drawSettings();
        fly(1.5);
        auto w = harness::draw();
        check("the host reconnected to its own server with the password",
              shows(w, "connected to 127.0.0.1"));
        if (!shows(w, "connected to 127.0.0.1")) dump(w);

        Peer stranger("NOPASS", 57.86, 27.03);
        check("a pilot without the password is refused", !stranger.login(""));
        check("...and told why", stranger.rejectedWith() == xr::RJ_PASSWORD,
              std::to_string(stranger.rejectedWith()));
        Peer wrong("WRONGPW", 57.86, 27.03);
        check("a wrong password is refused", !wrong.login("nimbus") &&
                                             wrong.rejectedWith() == xr::RJ_PASSWORD);
        Peer friendly("FRIEND2", 57.86, 27.03);
        check("the right password gets in", friendly.login("cumulus"));
    }

    printf("\nswitching hosting off again\n");
    {
        // Untick it and save: the server must stop and let the port go.
        harness::menu(1);
        drawSettings();
        int tx = 0, ty = 0;
        harness::drawnAt("Hosting", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();
        int lx = 0, ly = 0, top = 0, left = 0;
        harness::windowTop(kWin, &top, &left);
        harness::drawnAt("Host a flight here", &lx, &ly);
        harness::click(kWin, left + xr::ui::Ctx::kValueX + 10, ly);
        drawSettings();
        int bx = 0, by = 0;
        harness::drawnAt("Save & apply", &bx, &by);
        harness::click(kWin, bx + 20, by);
        drawSettings();
        fly(0.4);

        auto w = harness::draw();
        check("the main window stops saying it is hosting", !shows(w, "Hosting  ·"));

        Peer late("TOOLATE", 57.86, 27.03);
        check("nobody can join any more", !late.login());
    }

    XPluginDisable();
    XPluginStop();

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all hosting tests passed\n");
    return 0;
}
