// Renders the new window to PNG files without X-Plane: the same drawMain()/drawOverlay() as in the sim, a
// sample model, and a small software rasteriser for ImGui's triangles (textured with the font atlas).
// Tower mode's windows too, from tools/fixtures/tower_preview.jsonl (made by tools/fake_towertrainer.py --dump).
// Used for the screenshots in docs/ui/ and to see the layout before it ever runs in the sim.
//   ui_preview <outdir> [theme] [tower fixture]     -> <outdir>/<tab>.png, <outdir>/tower-<window>.png
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "../../plugin/src/ui2/app_ui.h"
#include "../../plugin/src/ui2/i18n.h"
#include "../../plugin/src/ui2/theme.h"
#include "../../plugin/src/ui2/font_inter.h"
#include "../../plugin/src/ui2/tower_ui.h"
#include "../../plugin/src/tower_parse.h"
#include "png.h"

namespace {

struct Raster {
    int w, h;
    std::vector<unsigned char> rgba;
    const unsigned char* tex = nullptr; int tw = 0, th = 0;
    Raster(int W, int H) : w(W), h(H), rgba((size_t)W * H * 4, 0) {}
    void clear(ImVec4 c) { for (int i = 0; i < w * h; ++i) { rgba[i * 4] = (unsigned char)(c.x * 255); rgba[i * 4 + 1] = (unsigned char)(c.y * 255); rgba[i * 4 + 2] = (unsigned char)(c.z * 255); rgba[i * 4 + 3] = 255; } }
    float sample(float u, float v) const {
        if (!tex) return 1.f;
        int x = (int)(u * tw), y = (int)(v * th);
        x = std::max(0, std::min(tw - 1, x)); y = std::max(0, std::min(th - 1, y));
        return tex[(y * tw + x) * 4 + 3] / 255.f;
    }
    void tri(const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, const ImVec4& clip) {
        const float minx = std::max(clip.x, std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))), maxx = std::min(clip.z, std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x})));
        const float miny = std::max(clip.y, std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))), maxy = std::min(clip.w, std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y})));
        const float det = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
        if (std::fabs(det) < 1e-6f) return;
        for (int y = (int)miny; y < (int)maxy; ++y) {
            if (y < 0 || y >= h) continue;
            for (int x = (int)minx; x < (int)maxx; ++x) {
                if (x < 0 || x >= w) continue;
                const float px = x + 0.5f, py = y + 0.5f;
                float l0 = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / det;
                float l1 = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / det;
                float l2 = 1.f - l0 - l1;
                if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
                const float u = l0 * a.uv.x + l1 * b.uv.x + l2 * c.uv.x, v = l0 * a.uv.y + l1 * b.uv.y + l2 * c.uv.y;
                const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a.col), cb = ImGui::ColorConvertU32ToFloat4(b.col), cc = ImGui::ColorConvertU32ToFloat4(c.col);
                const float r = l0 * ca.x + l1 * cb.x + l2 * cc.x, g = l0 * ca.y + l1 * cb.y + l2 * cc.y, bl = l0 * ca.z + l1 * cb.z + l2 * cc.z;
                const float al = (l0 * ca.w + l1 * cb.w + l2 * cc.w) * sample(u, v);
                if (al <= 0.002f) continue;
                unsigned char* d = &rgba[((size_t)y * w + x) * 4];
                d[0] = (unsigned char)(d[0] * (1 - al) + r * 255 * al); d[1] = (unsigned char)(d[1] * (1 - al) + g * 255 * al); d[2] = (unsigned char)(d[2] * (1 - al) + bl * 255 * al);
            }
        }
    }
    void draw(ImDrawData* dd) {
        for (int n = 0; n < dd->CmdListsCount; ++n) {
            const ImDrawList* cl = dd->CmdLists[n];
            for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
                const ImDrawCmd& cmd = cl->CmdBuffer[c];
                const ImDrawIdx* idx = cl->IdxBuffer.Data + cmd.IdxOffset;
                for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) tri(cl->VtxBuffer[idx[i]], cl->VtxBuffer[idx[i + 1]], cl->VtxBuffer[idx[i + 2]], cmd.ClipRect);
            }
        }
    }
};

xr::ui2::Model sampleModel(double t) {
    using namespace xr::ui2;
    Model m;
    m.elapsed = t;
    m.conn.connected = true; m.conn.status = "connected to 192.168.1.20:49100"; m.conn.endpoint = "192.168.1.20:49100"; m.conn.callsign = "ESXYZ"; m.conn.icao = "C172"; m.conn.livery = "Estonian Flying Club";
    m.conn.protoLocal = 6; m.conn.protoServer = 6; m.conn.lastPacketS = 0.3f; m.conn.packetsPerS = 24; m.conn.peers = 3; m.conn.aiCount = 12; m.conn.skyLine = "the flight's (following)";
    m.com[0] = {1, 135905, 131905, true, "Tallinn Tower", true, false, 0.8f, 0.85f};
    m.com[1] = {2, 124880, 121500, true, "Tallinn Info (ATIS)", false, false, 0.f, 0.6f};
    m.txWho = "FIN321";
    m.atc = {{"EETN_TWR", "Tower", "EETN", 135905, 4.2, true, true, false}, {"EETN_GND", "Ground", "EETN", 131905, 4.2, true, false, false}, {"EETN_APP", "Approach", "EETN", 127905, 4.2, true, false, false}, {"EETN_ATIS", "ATIS", "EETN", 124880, 4.2, true, true, true}, {"EFHK_TWR", "Tower", "EFHK", 118700, 48.0, false, false, false}};
    m.atis = {{"EETN information C", "Tallinn information Charlie, time 0620, runway in use 26, ILS approach, transition level 70, wind 200 degrees 6 knots, visibility 10 kilometres, overcast 700 feet, temperature 9, dew point 7, QNH 1022. Acknowledge information Charlie on first contact."}};
    m.pilots = {{1, "FIN321", "A320", 2.1, 1200, 180, 135905, 4521, true, false, true, false, 0.8f}, {2, "SAS123", "B738", 6.8, 3400, 250, 135905, 4522, false, false, true, false, 0.5f}, {3, "DLH5KM", "A321", 14.0, 9000, 300, 127905, 4530, false, false, true, false, 0.3f},
                {4, "BTI101", "BCS3", 0.9, 131, 0, 131905, 7000, false, true, true, false, 0.9f}, {5, "N771AB", "C172", 22.0, 2500, 95, 0, 7700, false, false, true, true, 0.1f}};
    m.messages = {{t - 300, "", "connected to 192.168.1.20:49100 as ESXYZ", "0:30", 0, false, false, false, false, true},
                  {t - 240, "EETN_TWR", "Finnair 321, Tallinn Tower, runway 26, cleared for takeoff, wind 200 degrees 6 knots", "1:30", 135905, true, false, false, false, false},
                  {t - 230, "FIN321", "Runway 26, cleared for takeoff, Finnair 321", "1:40", 135905, false, false, false, false, false},
                  {t - 120, "ESXYZ", "Tallinn Tower, ESXYZ, holding point runway 26, ready for departure", "3:30", 135905, false, false, true, false, false},
                  {t - 110, "EETN_TWR", "ESXYZ, hold position, traffic on short final", "3:40", 135905, true, false, false, false, false},
                  {t - 40, "SAS123", "did you see the weather over Riga?", "4:50", 135905, false, true, false, false, false}};
    m.fp.rules = "I"; m.fp.dep = "EETN"; m.fp.dest = "EVRA"; m.fp.altn = "EYVI"; m.fp.type = "C172"; m.fp.callsign = "ESXYZ"; m.fp.route = "ULGOT N746 ABETI"; m.fp.cruise = "5000"; m.fp.tas = "110"; m.fp.eobt = "0730"; m.fp.equip = "SDFGY/S"; m.fp.remarks = "PBN/B2 SIMBRIEF";
    m.fp.status = 2; m.fp.squawk = 4521; m.fp.clearedSid = "ULGOT1G"; m.fp.clearedLevel = "5000 ft"; m.fp.clearedDest = "EVRA"; m.fp.simbriefSummary = "OFP TTR123 EETN-EVRA FL360 (2 min ago)";
    m.fp.clearance = "ESXYZ, cleared to Riga via ULGOT1G departure, climb 5000 feet, squawk 4521";
    m.fp.ofp.present = true; m.fp.ofp.callsign = "TTR123"; m.fp.ofp.type = "A320"; m.fp.ofp.reg = "ESTTR"; m.fp.ofp.dep = "EETN"; m.fp.ofp.dest = "EVRA"; m.fp.ofp.altn = "EYVI";
    m.fp.ofp.depRwy = "08"; m.fp.ofp.destRwy = "18"; m.fp.ofp.sid = "ULGOT1G"; m.fp.ofp.star = "ABETI2A"; m.fp.ofp.route = "ULGOT1G ULGOT N746 ABETI ABETI2A"; m.fp.ofp.cruise = "FL360"; m.fp.ofp.tas = "447"; m.fp.ofp.mach = "M0.78";
    m.fp.ofp.eobt = "0730"; m.fp.ofp.etd = "0745"; m.fp.ofp.eta = "0836"; m.fp.ofp.block = "4600"; m.fp.ofp.trip = "2100"; m.fp.ofp.zfw = "56000"; m.fp.ofp.tow = "60500"; m.fp.ofp.ldw = "58400"; m.fp.ofp.units = "kg"; m.fp.ofp.pdfUrl = "https://www.simbrief.com/ofp/flightplans/xml/TTR123.pdf";
    m.settings.simbriefUser = "tester";
    m.host.running = true; m.host.port = 49100; m.host.clients = 3; m.host.address = "203.0.113.17:49100"; m.host.joinCode = "K7M2Q-X4PB9"; m.host.lanAddress = "192.168.1.20:49100"; m.host.mapped = true; m.host.router = "UPnP"; m.host.callsigns = {"FIN321", "SAS123", "DLH5KM"};
    m.voice.available = true; m.voice.haveMic = true; m.voice.status = "OK  mic: Headset Microphone"; m.voice.micLevel = 0.45f; m.voice.micDevices = {"Headset Microphone", "Webcam"}; m.voice.outDevices = {"Headset", "Speakers"};
    m.update.current = "0.6.0"; m.update.newer = false;
    m.towerActive = false; m.towerStatus = "off";
    m.settings.callsign = "ESXYZ"; m.settings.acIcao = "C172"; m.settings.host = "192.168.1.20"; m.settings.port = "49100"; m.settings.pttKey = 0x20; m.settings.hostEnabled = true; m.settings.uiWizardDone = true;
    m.readUntil = t - 200;
    return m;
}

// Draws a few frames into a fresh context (tables and child sizes settle) and writes the last one.
void renderFrames(const std::string& path, int w, int h, const std::string& theme, bool overlay, ImFontAtlas* atlas, const unsigned char* tex, int tw, int th,
                  const std::function<void()>& draw) {
    using namespace xr::ui2;
    ImGuiContext* ctx = ImGui::CreateContext(atlas);
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2((float)w, (float)h); io.DeltaTime = 1.f / 60.f;
    applyTheme(theme.c_str(), 1.f);
    Raster r(w, h); r.tex = tex; r.tw = tw; r.th = th;
    r.clear(overlay ? ImVec4(0.25f, 0.3f, 0.36f, 1) : palette().bg);
    for (int frame = 0; frame < 3; ++frame) {   // a few frames: tables and child sizes settle
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2((float)w, (float)h));
        ImGui::Begin("##xr", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | (overlay ? ImGuiWindowFlags_NoBackground : 0));
        Fonts f; f.body = atlas->Fonts[0]; f.heading = atlas->Fonts[1]; f.big = atlas->Fonts[2]; f.mono = atlas->Fonts[0]; f.small = atlas->Fonts[3];
        setFonts(f);
        setTowerFonts(f);
        ImGui::PushFont(atlas->Fonts[0]);
        draw();
        ImGui::PopFont();
        ImGui::End();
        ImGui::Render();
        if (frame == 2) r.draw(ImGui::GetDrawData());
    }
    writePng(path, w, h, r.rgba.data());
    ImGui::DestroyContext(ctx);
    printf("wrote %s\n", path.c_str());
}

void renderTo(const std::string& path, int w, int h, xr::ui2::Model& m, const xr::ui2::Actions& a, bool overlay, ImFontAtlas* atlas, const unsigned char* tex, int tw, int th) {
    renderFrames(path, w, h, m.settings.uiTheme, overlay, atlas, tex, tw, th, [&] { if (overlay) xr::ui2::drawOverlay(m, a, (float)w); else xr::ui2::drawMain(m, a); });
}

// Tower mode as it looks linked to a trainer: the fixture's airport and state, FIN321 selected, a pilot on the air.
bool loadTower(const std::string& path, xr::tower::State* s) {
    std::ifstream in(path);
    std::string line;
    bool airport = false, state = false;
    s->myId = "x1";
    while (std::getline(in, line)) {
        airport |= xr::tower::parseAirportLine(line, *s);
        state |= xr::tower::parseStateLine(line, *s, "TWR");
    }
    s->phase = xr::tower::Phase::Linked; s->linked = true; s->endpoint = "127.0.0.1:8781";
    s->now = 100.f; s->selectedId = 1;
    xr::tower::LiveCall c; c.from = "DLH5KM"; c.text = "Lufthansa five kilo mike, 3 miles, runway 26"; c.at = 99.f;
    s->live.push_back(c);
    return airport && state;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : "docs/ui";
    const std::string theme = argc > 2 ? argv[2] : "dark";
    ImFontAtlas atlas;
    ImFontConfig cfg; cfg.OversampleH = 2;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x2027, 0x2190, 0x21FF, 0x2500, 0x25FF, 0};
    atlas.AddFontFromMemoryCompressedTTF(InterRegular_compressed_data, (int)InterRegular_compressed_size, 15.f, &cfg, ranges);
    atlas.AddFontFromMemoryCompressedTTF(InterSemiBold_compressed_data, (int)InterSemiBold_compressed_size, 17.f, &cfg, ranges);
    atlas.AddFontFromMemoryCompressedTTF(InterSemiBold_compressed_data, (int)InterSemiBold_compressed_size, 26.f, &cfg, ranges);
    atlas.AddFontFromMemoryCompressedTTF(InterRegular_compressed_data, (int)InterRegular_compressed_size, 12.5f, &cfg, ranges);
    unsigned char* pixels = nullptr; int tw = 0, th = 0;
    atlas.GetTexDataAsRGBA32(&pixels, &tw, &th);
    std::vector<unsigned char> tex(pixels, pixels + (size_t)tw * th * 4);
    atlas.SetTexID((ImTextureID)1);
    xr::ui2::Actions a;    // no-op actions: the preview only draws
    const char* tabs[8] = {"radio", "atc", "pilots", "messages", "flightplan", "host", "settings", "tower"};
    for (int i = 0; i < 8; ++i) {
        xr::ui2::Model m = sampleModel(400.0);
        m.settings.uiTheme = theme;
        xr::ui2::state() = xr::ui2::UiState();
        xr::ui2::state().tab = i;
        if (i == 6) xr::ui2::state().settingsGroup = 0;
        renderTo(out + "/" + tabs[i] + (theme == "light" ? "-light" : "") + ".png", 820, 540, m, a, false, &atlas, tex.data(), tw, th);
    }
    {   // the wizard
        xr::ui2::Model m = sampleModel(400.0); m.settings.uiTheme = theme; m.settings.uiWizardDone = false; m.settings.callsign.clear();
        xr::ui2::state() = xr::ui2::UiState(); xr::ui2::state().wizardOpen = true;
        renderTo(out + "/wizard" + (theme == "light" ? "-light" : "") + ".png", 820, 540, m, a, false, &atlas, tex.data(), tw, th);
    }
    {   // the overlay with a toast
        xr::ui2::Model m = sampleModel(400.0); m.settings.uiTheme = theme; m.pttDown = false;
        xr::ui2::state() = xr::ui2::UiState();
        xr::ui2::toast("EETN_TWR: ESXYZ, hold position, traffic on short final", 2, 399.0);
        renderTo(out + "/overlay" + (theme == "light" ? "-light" : "") + ".png", 560, 96, m, a, true, &atlas, tex.data(), tw, th);
    }
    {   // Tower mode's windows
        const std::string fixture = argc > 3 ? argv[3] : "tools/fixtures/tower_preview.jsonl";
        xr::tower::State s;
        if (!loadTower(fixture, &s)) { fprintf(stderr, "no tower fixture at %s -- run from the repo root or pass its path\n", fixture.c_str()); return 1; }
        xr::ui2::TowerActions ta;
        const char* names[xr::ui2::kTwCount] = {"radar", "strips", "delivery", "frequency", "ops", "banner"};
        const int sizes[xr::ui2::kTwCount][2] = {{960, 620}, {560, 560}, {760, 250}, {720, 330}, {640, 360}, {640, 128}};
        for (int i = 0; i < xr::ui2::kTwCount; ++i) {
            xr::tower::State v = s;
            if (i == xr::ui2::kTwBanner) { v.phase = xr::tower::Phase::Unreachable; v.linkError = "connection refused"; v.retryInS = 4.f; }
            xr::ui2::towerUi() = xr::ui2::TowerUi();
            if (i == xr::ui2::kTwFrequency) snprintf(xr::ui2::towerUi().cmd, sizeof xr::ui2::towerUi().cmd, "FIN");
            xr::ui2::TowerFrame f; f.which = i; f.code = "TOWER"; f.position = "TWR";
            renderFrames(out + "/tower-" + names[i] + (theme == "light" ? "-light" : "") + ".png", sizes[i][0], sizes[i][1], theme, false, &atlas, tex.data(), tw, th,
                         [&] { xr::ui2::drawTower(f, v, ta); });
        }
        {   // the radar close up: the apron, with taxiway names, stand names and aircraft at their real size
            xr::ui2::towerUi() = xr::ui2::TowerUi();
            xr::ui2::TowerUi& u = xr::ui2::towerUi();
            u.view.cx = 1050.f; u.view.cy = -230.f; u.view.ppm = 1.15f; u.viewIcao = s.icao; u.viewFitted = true;
            xr::ui2::TowerFrame f; f.which = xr::ui2::kTwRadar;
            renderFrames(out + "/tower-radar-close" + (theme == "light" ? "-light" : "") + ".png", 960, 620, theme, false, &atlas, tex.data(), tw, th,
                         [&] { xr::ui2::drawTower(f, s, ta); });
        }
    }
    return 0;
}
