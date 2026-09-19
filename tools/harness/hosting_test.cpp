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

    bool login() {
        xr::LoginPayload lp{};
        snprintf(lp.callsign, sizeof(lp.callsign), "%s", callsign_.c_str());
        snprintf(lp.acIcao, sizeof(lp.acIcao), "%s", "B738");
        lp.protoVer = xr::kProtoVersion;
        send(xr::PT_LOGIN, 0, &lp, sizeof(lp));

        uint8_t buf[xr::kMaxPacket];
        for (int i = 0; i < 25; ++i) {
            const int n = recvAny(buf, sizeof(buf));
            if (n >= (int)(sizeof(xr::Header) + sizeof(xr::LoginAckPayload))) {
                xr::Header h{};
                memcpy(&h, buf, sizeof(h));
                if (h.type == xr::PT_LOGIN_ACK) {
                    xr::LoginAckPayload ack{};
                    memcpy(&ack, buf + sizeof(h), sizeof(ack));
                    sid_ = ack.sessionId;
                    return sid_ != 0;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        return false;
    }

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
        std::vector<std::string> texts;
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
        s.acIcao = "C172";
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
    harness::set("sim/cockpit2/radios/actuators/audio_com_selection", 6);
    harness::set("sim/flightmodel/position/elevation", 914.0);

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

    printf("\nswitching hosting off again\n");
    {
        // Untick it and save: the server must stop and let the port go.
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
