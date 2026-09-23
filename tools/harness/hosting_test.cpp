// Hosting, end to end: the plugin runs the server, its own client joins it,
// and a real peer connects over real UDP -- no Python, no VPS, nothing to
// configure. This is the whole point of the feature, so it is tested the way
// a pilot would experience it: switch it on in the settings window, then see
// whether someone else can actually get in.
#include "harness.h"
#include "protocol.h"
#include "settings.h"
#include "ui.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
        p.squawk = squawk_;
        p.xpdrMode = xpdrMode_;
        p.xpdrIdent = ident_;
        send(xr::PT_POSITION, sid_, &p, sizeof(p));
    }

    // How this pilot's transponder is set. The default is a VFR aircraft
    // squawking mode C, because that is what almost every test wants and an
    // aircraft with its transponder off is deliberately invisible to TCAS.
    void transponder(uint16_t squawk, uint8_t mode, uint8_t ident = 0) {
        squawk_ = squawk; xpdrMode_ = mode; ident_ = ident;
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
        std::vector<xr::TrafficEntry> entries;
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
                    bag.entries.push_back(e);
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
    uint16_t    squawk_ = 1200;
    uint8_t     xpdrMode_ = xr::XPDR_ALT;
    uint8_t     ident_ = 0;
    uint32_t    clock_ = 0;
    sockaddr_in dst_{};
};

static bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) if (x == s) return true;
    return false;
}

// Is this callsign on the traffic list, as opposed to merely somewhere in
// the window? Radio messages carry a callsign too, and an aircraft that has
// dropped off TCAS can still be talking -- which is the whole point of the
// transponder tests, so they must not confuse the two.
static std::string trafficRow(const std::vector<std::string>& w,
                              const std::string& callsign) {
    for (const auto& line : w)
        if (line.find(" nm") != std::string::npos &&
            line.find(callsign) != std::string::npos) return line;
    return "";
}
static bool onTrafficList(const std::vector<std::string>& w, const std::string& callsign) {
    return !trafficRow(w, callsign).empty();
}

// The newest traffic entry for one callsign, or nothing.
static const xr::TrafficEntry* entryFor(const Peer::Bag& bag, const char* callsign) {
    const xr::TrafficEntry* found = nullptr;
    for (const auto& e : bag.entries) {
        char cs[17] = {0};
        memcpy(cs, e.callsign, 16);
        if (!strcmp(cs, callsign)) found = &e;
    }
    return found;
}

// ---------------------------------------------------------------------------
// A server that is nobody's X-Plane: a VPS, in other words.
// ---------------------------------------------------------------------------
// It speaks just enough of the protocol to let the plugin in and keep it
// there, and it writes down what the plugin offered on the way past. The
// point is what it has not got: a sim, and therefore any weather of its own.
class FakeServer {
public:
    explicit FakeServer(uint16_t port) {
        fd_ = (int)socket(AF_INET, SOCK_DGRAM, 0);
        timeval tv{0, 100 * 1000};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_port = htons(port);
        me.sin_addr.s_addr = inet_addr("127.0.0.1");
        ok_ = ::bind(fd_, (const sockaddr*)&me, sizeof(me)) == 0;
        if (ok_) worker_ = std::thread(&FakeServer::run, this);
    }
    ~FakeServer() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
        if (fd_ >= 0) CLOSESOCK(fd_);
    }

    bool ok()       const { return ok_; }
    int  logins()   const { return logins_.load(); }
    int  claims()   const { return claims_.load(); }   // logins offering the sky
    int  weathers() const { return weathers_.load(); }
    void reset() { logins_ = 0; claims_ = 0; weathers_ = 0; }

    // Relay a sky, as the real server would on behalf of whoever holds the
    // claim. The plugin should read this as "not me, then".
    void pushWeather(const xr::WeatherPayload& w) {
        std::lock_guard<std::mutex> lk(mx_);
        pending_ = w;
        havePending_ = true;
    }

private:
    void run() {
        uint8_t buf[xr::kMaxPacket];
        while (!stop_) {
            sockaddr_in from{};
#ifdef _WIN32
            int fromLen = (int)sizeof(from);
#else
            socklen_t fromLen = sizeof(from);
#endif
            const int n = (int)recvfrom(fd_, (char*)buf, sizeof(buf), 0,
                                        (sockaddr*)&from, &fromLen);
            if (n >= (int)sizeof(xr::Header)) {
                xr::Header h{};
                memcpy(&h, buf, sizeof(h));
                if (h.magic == xr::kMagic) {
                    std::lock_guard<std::mutex> lk(mx_);
                    peer_ = from;
                    havePeer_ = true;
                    if (h.type == xr::PT_LOGIN &&
                        n >= (int)(sizeof(h) + sizeof(xr::LoginPayload))) {
                        xr::LoginPayload lp{};
                        memcpy(&lp, buf + sizeof(h), sizeof(lp));
                        ++logins_;
                        if (lp.flags & xr::LF_WEATHER_SOURCE) ++claims_;
                        xr::LoginAckPayload ack{};
                        ack.sessionId = sid_;
                        ack.serverTimeMs = 0;
                        send(xr::PT_LOGIN_ACK, &ack, (int)sizeof(ack));
                    } else if (h.type == xr::PT_WEATHER) {
                        ++weathers_;
                    } else if (h.type == xr::PT_POSITION) {
                        // An empty traffic reply: the plugin drops a server
                        // that has gone quiet, and this one must not look
                        // like it has.
                        xr::TrafficHeader th{};
                        send(xr::PT_TRAFFIC, &th, (int)sizeof(th));
                    }
                }
            }
            std::lock_guard<std::mutex> lk(mx_);
            if (havePending_ && havePeer_) {
                havePending_ = false;
                send(xr::PT_WEATHER, &pending_, (int)sizeof(pending_));
            }
        }
    }

    // Call with mx_ held and peer_ known.
    void send(uint8_t type, const void* payload, int len) {
        uint8_t out[xr::kMaxPacket];
        xr::Header h{};
        h.magic = xr::kMagic;
        h.type = type;
        h.version = (uint8_t)xr::kProtoVersion;
        h.payloadLen = (uint16_t)len;
        h.sessionId = sid_;
        memcpy(out, &h, sizeof(h));
        memcpy(out + sizeof(h), payload, (size_t)len);
        sendto(fd_, (const char*)out, sizeof(h) + (size_t)len, 0,
               (const sockaddr*)&peer_, sizeof(peer_));
    }

    static const uint32_t sid_ = 7;
    int                fd_ = -1;
    bool               ok_ = false;
    std::atomic<bool>  stop_{false};
    std::atomic<int>   logins_{0}, claims_{0}, weathers_{0};
    std::thread        worker_;
    std::mutex         mx_;
    sockaddr_in        peer_{};
    bool               havePeer_ = false;
    xr::WeatherPayload pending_{};
    bool               havePending_ = false;
};

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
        // Nothing is listening here yet, so the plugin cannot connect until
        // hosting is switched on and repoints it -- which is what the early
        // sections want. The last section binds a fake dedicated server to
        // this port and switches hosting off again, and the plugin finds it.
        s.port = std::to_string(g_port + 1);
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
    // Transponder on and squawking mode C: without one, TCAS has nothing to
    // interrogate with and the traffic list is empty by design.
    harness::set("sim/cockpit2/radios/actuators/transponder_mode", xr::XPDR_ALT);
    harness::set("sim/cockpit2/radios/actuators/transponder_code", 4321);
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

        // Squash the window down to where nothing could possibly fit, then
        // come back to this tab: a pilot who has dragged the window small
        // must not be left with the join code and the buttons drawn over the
        // cockpit below it. Going away and returning is what a pilot does,
        // and it is also what tells the window its content changed size.
        int l0 = 0, t0 = 0, r0 = 0, b0 = 0;
        harness::windowRect(kWin, &l0, &t0, &r0, &b0);
        harness::setWindowRect(kWin, l0, t0, r0, t0 - 200);
        // fly() draws the main window, so the recorded positions are its
        // rows, not this window's: draw the settings window before asking it
        // where its tabs are.
        drawSettings();
        harness::drawnAt("Audio", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();
        harness::drawnAt("Hosting", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        auto v = drawSettings();
        v = drawSettings();
        int l1 = 0, t1 = 0, r1 = 0, b1 = 0;
        harness::windowRect(kWin, &l1, &t1, &r1, &b1);
        check("a squashed window grows back to fit the hosting tab",
              (t1 - b1) > 200,
              "200 -> " + std::to_string(t1 - b1) + " px");

        check("it says the server is running", shows(v, "Running on port"));
        check("it shows an address to give out", shows(v, "Friends type:"));
        check("it names who is connected", shows(v, "HOSTER") && shows(v, "FRIEND"));
        check("it does not claim the router opened anything",
              !shows(v, "Router opened"));
        // A pilot whose router cannot be changed at all -- an ISP box with no
        // settings page, a carrier's NAT -- must not be left thinking port
        // forwarding is the only way to fly with friends.
        check("it offers a way out when the router cannot be changed",
              shows(v, "Tailscale") || shows(v, "let a friend host"));

        // This tab says far more than the others, and a window sized for the
        // Connection tab used to cut off the join code, who is connected,
        // and the buttons -- drawing them over the cockpit below the window.
        int wl = 0, wt = 0, wr = 0, wb = 0;
        harness::windowRect(kWin, &wl, &wt, &wr, &wb);
        int below = 0;
        for (const auto& d : harness::drawnPositions())
            if (d.y < wb) ++below;
        check("nothing is drawn below the window", below == 0,
              std::to_string(below) + " lines");
        check("the buttons are still there", shows(v, "Save & apply") && shows(v, "Cancel"));
        if (below || !shows(v, "Save & apply")) dump(v);
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

    printf("\nthe transponder decides who is on TCAS\n");
    {
        // TCAS works by interrogating transponders. An aircraft whose
        // transponder is off or in standby does not answer, so it is not a
        // contact -- and neither is anybody, if it is our own transponder
        // that is off, because the interrogation comes from us. None of this
        // touches the radio or the model: you can see and talk to an
        // aircraft that is squawking nothing at all.
        Peer sqk("SQK001", 57.858, 27.028);
        check("a pilot joins", sqk.login());

        sqk.transponder(4671, xr::XPDR_ALT);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        auto w = harness::draw();
        std::string row = trafficRow(w, "SQK001");
        check("squawking mode C, they are a contact", !row.empty());
        check("their squawk is on their own row",
              row.find("4671") != std::string::npos, row);
        check("with their altitude", row.find("2999 ft") != std::string::npos, row);
        check("and our own transponder is on the traffic line",
              shows(w, "XPDR 4321 ALT"));
        if (!shows(w, "4671")) dump(w);

        // Mode A answers with an identity and no altitude at all.
        sqk.transponder(4671, xr::XPDR_ON);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        w = harness::draw();
        row = trafficRow(w, "SQK001");
        check("on mode A they are still a contact", !row.empty());
        check("but with no altitude to report",
              row.find("no alt") != std::string::npos, row);
        check("and no invented one", row.find(" ft") == std::string::npos, row);
        if (row.find("no alt") == std::string::npos) dump(w);

        // IDENT, the button a pilot presses when ATC asks them to.
        sqk.transponder(4671, xr::XPDR_ALT, 1);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        check("squawking ident is visible",
              trafficRow(harness::draw(), "SQK001").find(" ID") != std::string::npos,
              trafficRow(harness::draw(), "SQK001"));

        // Standby: no answer, so no contact.
        sqk.transponder(4671, xr::XPDR_STANDBY);
        for (int i = 0; i < 8; ++i) { sqk.position(); fly(0.1); }
        w = harness::draw();
        check("in standby they are off TCAS", !onTrafficList(w, "SQK001"));
        if (onTrafficList(w, "SQK001")) dump(w);

        // ...but they are still on the radio. A transponder is not a radio.
        sqk.text("standby but still talking");
        fly(0.8);
        check("a transponder has nothing to do with the radio",
              shows(harness::draw(), "standby but still talking"));

        // Our own transponder off: TCAS is inoperative, so nobody shows.
        sqk.transponder(4671, xr::XPDR_ALT);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        check("back on mode C they return", onTrafficList(harness::draw(), "SQK001"));

        harness::set("sim/cockpit2/radios/actuators/transponder_mode", xr::XPDR_STANDBY);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        w = harness::draw();
        check("with our own transponder in standby, TCAS is blank",
              !onTrafficList(w, "SQK001"));
        check("and it says why, rather than looking broken",
              shows(w, "TCAS off -- XPDR STBY"));
        if (!shows(w, "TCAS off -- XPDR STBY")) dump(w);

        harness::set("sim/cockpit2/radios/actuators/transponder_mode", xr::XPDR_ALT);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        check("switching it back brings the traffic back",
              onTrafficList(harness::draw(), "SQK001"));

        // A cold and dark aircraft is not squawking either: the transponder
        // is on the same bus as the radios.
        harness::set("sim/cockpit2/switches/avionics_power_on", 0);
        harness::set("sim/cockpit2/electrical/bus_volts", 0.0);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        check("with no power our transponder is off, whatever the knob says",
              shows(harness::draw(), "XPDR OFF"));
        harness::set("sim/cockpit2/switches/avionics_power_on", 1);
        harness::set("sim/cockpit2/electrical/bus_volts", 24.0);
        fly(0.4);

        // A squawk is four octal digits. 7788 is not one.
        sqk.transponder(7788, xr::XPDR_ALT);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        check("an impossible squawk is not relayed as if it were real",
              trafficRow(harness::draw(), "SQK001").find("7788") == std::string::npos,
              trafficRow(harness::draw(), "SQK001"));

        // An emergency squawk should catch the eye, not hide in the list.
        sqk.transponder(7700, xr::XPDR_ALT);
        for (int i = 0; i < 6; ++i) { sqk.position(); fly(0.1); }
        check("an emergency squawk is shown",
              trafficRow(harness::draw(), "SQK001").find("7700") != std::string::npos,
              trafficRow(harness::draw(), "SQK001"));
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

    printf("\non the ground we report the ground, not our own gear height\n");
    {
        // XPMP2 stands a CSL model on its wheels by adding that model's
        // VERT_OFFSET to whatever altitude it is handed. X-Plane's
        // `elevation` is the aircraft's datum point, which on the ground is
        // already a couple of metres up, on top of its own gear -- so
        // sending that has the gear counted twice, and a pilot watching the
        // apron sees everybody hovering. What the receiver needs while we
        // are down is the ground.
        Peer obs("GNDOBS", 57.858, 27.028);
        check("an observer joins", obs.login());

        // Some altitude bookkeeping that has to survive the round trip: a
        // stand at 100 m, with our datum 2.4 m above it.
        harness::set("sim/flightmodel/position/elevation", 100.0);
        harness::set("sim/flightmodel/position/y_agl", 2.4);
        harness::set("sim/flightmodel/failures/onground_any", 1);
        for (int i = 0; i < 8; ++i) { obs.position(); fly(0.1); }
        auto bag = obs.drain(400);
        const xr::TrafficEntry* me = entryFor(bag, "HOSTER");
        check("the observer sees us", me != nullptr);
        if (me) {
            check("parked, we are reported at ground level",
                  std::fabs(me->altMslM - 97.6f) < 0.05f,
                  std::to_string(me->altMslM) + " m, wanted 97.6");
            check("and still flagged as on the ground", me->onGround == 1);
        }

        // Airborne, the datum is the aircraft, so it goes out untouched and
        // the receiver drops the offset instead (see xpmp_bridge.cpp).
        harness::set("sim/flightmodel/position/elevation", 900.0);
        harness::set("sim/flightmodel/position/y_agl", 800.0);
        harness::set("sim/flightmodel/failures/onground_any", 0);
        for (int i = 0; i < 8; ++i) { obs.position(); fly(0.1); }
        bag = obs.drain(400);
        me = entryFor(bag, "HOSTER");
        check("in the air, our real altitude is sent", me != nullptr &&
              std::fabs(me->altMslM - 900.f) < 0.05f,
              me ? std::to_string(me->altMslM) + " m, wanted 900" : "no entry");

        // A nonsense reading must not bury the model: that would look far
        // worse than the hover it replaces.
        harness::set("sim/flightmodel/position/elevation", 100.0);
        harness::set("sim/flightmodel/position/y_agl", 4000.0);
        harness::set("sim/flightmodel/failures/onground_any", 1);
        for (int i = 0; i < 8; ++i) { obs.position(); fly(0.1); }
        bag = obs.drain(400);
        me = entryFor(bag, "HOSTER");
        check("an absurd AGL reading is ignored rather than applied",
              me != nullptr && std::fabs(me->altMslM - 100.f) < 0.05f,
              me ? std::to_string(me->altMslM) + " m, wanted 100" : "no entry");

        harness::set("sim/flightmodel/position/elevation", 914.0);
        harness::set("sim/flightmodel/position/y_agl", 0.0);
        harness::set("sim/flightmodel/failures/onground_any", 0);
        fly(0.4);
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
        harness::drawnAt("Traffic", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();
        int lx = 0, ly = 0;
        check("there is a share toggle", harness::drawnAt("Offer my weather", &lx, &ly));
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
        check("there is a follow toggle", harness::drawnAt("Fly the flight's", &lx, &ly));
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
        // Let the layout settle before reading a button's position: this tab
        // can grow the window, and a coordinate taken from the frame before
        // that would point at the wrong row -- which is what a pilot would
        // experience as a click that did nothing.
        drawSettings();
        int bx = 0, by = 0;
        harness::drawnAt("Save & apply", &bx, &by);
        harness::click(kWin, bx + 20, by);
        drawSettings();
        fly(0.4);

        // Two separate things can go wrong here, and they need telling
        // apart: the click never reaching the toggle, or the server not
        // stopping once it did. CI once failed this and the single check
        // could not say which.
        xr::Settings saved;
        xr::loadSettings(saved, kCfgPath);
        check("the setting was saved as off", !saved.hostEnabled);

        auto w = harness::draw();
        check("the main window stops saying it is hosting", !shows(w, "Hosting  ·"));
        if (shows(w, "Hosting  ·")) dump(w);

        Peer late("TOOLATE", 57.86, 27.03);
        check("nobody can join any more", !late.login());
    }

    printf("\nweather still has a source on a dedicated server\n");
    {
        // The bug: the offer to be the flight's weather source used to be
        // made only while this plugin was itself hosting. Join a server on a
        // VPS instead and nobody ever claimed it, so the server let nobody
        // send, and every pilot flew their own sky while the setting sat
        // there saying they were sharing.
        FakeServer srv((uint16_t)(g_port + 1));
        check("the dedicated server is listening", srv.ok());

        // Sharing was switched off earlier in this file, so start by
        // confirming the offer is not made when it should not be.
        fly(3.0);
        check("we reach the dedicated server", srv.logins() > 0,
              std::to_string(srv.logins()) + " logins");
        check("with sharing off, no claim is made", srv.claims() == 0,
              std::to_string(srv.claims()) + " claims");
        check("and no weather goes out", srv.weathers() == 0,
              std::to_string(srv.weathers()) + " packets");

        // Now switch it on, the way a pilot would.
        harness::menu(1);
        drawSettings();
        int tx = 0, ty = 0, top = 0, left = 0;
        harness::windowTop(kWin, &top, &left);
        harness::drawnAt("Traffic", &tx, &ty);
        harness::click(kWin, tx + 10, ty);
        drawSettings();
        int lx = 0, ly = 0;
        check("the share toggle is on the Traffic tab, not Hosting",
              harness::drawnAt("Offer my weather", &lx, &ly));
        harness::click(kWin, left + xr::ui::Ctx::kValueX + 10, ly);
        drawSettings();
        drawSettings();
        int bx = 0, by = 0;
        harness::drawnAt("Save & apply", &bx, &by);
        harness::click(kWin, bx + 20, by);
        drawSettings();

        {
            xr::Settings saved;
            xr::loadSettings(saved, kCfgPath);
            check("sharing is on in the saved settings", saved.shareWeather);
        }

        srv.reset();
        fly(3.0);
        check("the login now offers to be the weather source", srv.claims() > 0,
              std::to_string(srv.claims()) + " claims of " +
              std::to_string(srv.logins()) + " logins");
        check("and the sky actually goes out", srv.weathers() > 0,
              std::to_string(srv.weathers()) + " packets");

        auto w = harness::draw();
        check("the window says whose sky we are flying",
              shows(w, "Weather and time: mine"));
        if (!shows(w, "Weather and time: mine")) dump(w);

        // The other half: the server awards the claim to whoever asked
        // first, and does not tell the losers. A loser finds out by being
        // sent somebody else's sky -- and must then stop sending its own,
        // whether or not it is following.
        xr::WeatherPayload other{};
        other.zuluTimeSec = 36000.f;
        other.seaLevelPressurePa = 100500.f;
        other.visibilitySm = 6.f;
        srv.pushWeather(other);
        fly(1.0);
        srv.reset();
        fly(3.0);
        check("somebody else's sky arriving stops us sending ours",
              srv.weathers() == 0, std::to_string(srv.weathers()) + " packets");

        w = harness::draw();
        check("and the window says so", shows(w, "Weather and time: the flight's"));
        if (!shows(w, "Weather and time: the flight's")) dump(w);
    }

    XPluginDisable();
    XPluginStop();

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all hosting tests passed\n");
    return 0;
}
