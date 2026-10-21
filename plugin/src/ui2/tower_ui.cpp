#include "tower_ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "imgui.h"
#include "i18n.h"
#include "theme.h"

namespace xr {
namespace ui2 {

namespace {
TowerUi g_ui;
Fonts   g_fonts;
bool    g_probeOn = false;
std::map<std::string, ProbeRect> g_probes;
const std::vector<tower::Aircraft>* g_completionList = nullptr;   // the command line's Tab completion

constexpr float kPi = 3.14159265f;

// ---------------------------------------------------------------- small helpers
ImFont* font(void* f) { return f ? (ImFont*)f : ImGui::GetFont(); }
void pushFont(void* f) { if (f) ImGui::PushFont((ImFont*)f); }
void popFont(void* f) { if (f) ImGui::PopFont(); }
void muted(const char* s) { ImGui::TextColored(palette().muted, "%s", s); }
void heading(const char* s) { pushFont(g_fonts.heading); ImGui::TextUnformatted(s); popFont(g_fonts.heading); }
float textWidth(void* f, const char* s) { ImFont* ft = font(f); return ft->CalcTextSizeA(ft->FontSize, FLT_MAX, 0.f, s).x; }
void textAt(ImDrawList* dl, void* f, ImVec2 p, ImU32 c, const char* s) { ImFont* ft = font(f); dl->AddText(ft, ft->FontSize, p, c, s); }
float smallH() { return font(g_fonts.small)->FontSize; }
ImVec2 V(tw::Vec v) { return ImVec2(v.x, v.y); }
tw::Vec T(ImVec2 v) { return {v.x, v.y}; }
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
bool darkTheme() { const ImVec4& t = palette().text; return t.x + t.y + t.z > 1.5f; }
bool blinkOn(float now, float hz = 1.2f) { return std::fmod(now * hz, 1.f) < 0.55f; }
ImU32 fade(ImU32 c, float f) { return (c & ~IM_COL32_A_MASK) | ((ImU32)(((c >> IM_COL32_A_SHIFT) & 0xFF) * f) << IM_COL32_A_SHIFT); }

void probeItem(const std::string& name) {
    if (!g_probeOn) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    g_probes[name] = {a.x, a.y, b.x, b.y, true};
}
void probeRect(const std::string& name, ImVec2 a, ImVec2 b) { if (g_probeOn) g_probes[name] = {a.x, a.y, b.x, b.y, true}; }

const tower::Aircraft* findAc(const tower::State& s, int id) {
    for (const auto& a : s.ac) if (a.id == id) return &a;
    return nullptr;
}
std::string myPos(const tower::State& s, const TowerFrame& f) { return !s.myPos.empty() ? s.myPos : f.position; }

// The colours aircraft have everywhere: departures blue, arrivals amber, live pilots violet, vehicles grey,
// an emergency red.
ImVec4 kindColour(const tower::Aircraft& a) {
    const bool dark = darkTheme();
    for (const auto& w : a.warn) if (w == "EMG" || w == "RCF" || w == "HIJ") return palette().err;
    if (a.human) return dark ? ImVec4(0.78f, 0.59f, 1.f, 1.f) : ImVec4(0.50f, 0.25f, 0.80f, 1.f);
    if (a.veh) return palette().muted;
    if (a.kind == "arr") return dark ? ImVec4(1.f, 0.72f, 0.30f, 1.f) : ImVec4(0.80f, 0.45f, 0.f, 1.f);
    if (a.kind == "dep") return dark ? ImVec4(0.40f, 0.68f, 1.f, 1.f) : ImVec4(0.10f, 0.40f, 0.85f, 1.f);
    return palette().text;
}
ImVec4 senderColour(tw::Sender s) {
    const Palette& p = palette();
    tower::Aircraft probe;
    switch (s) {
        case tw::Sender::Atc: return p.atc;
        case tw::Sender::AiAtc: return mix(p.atc, p.muted, 0.45f);
        case tw::Sender::Departure: probe.kind = "dep"; return kindColour(probe);
        case tw::Sender::Arrival: probe.kind = "arr"; return kindColour(probe);
        case tw::Sender::LivePilot: probe.human = true; return kindColour(probe);
        case tw::Sender::Vehicle: return p.muted;
        case tw::Sender::Problem: return p.warn;
        case tw::Sender::Pilot: return p.text;
        default: return p.muted;
    }
}

// Put an aircraft in the command line: selected in the trainer, its callsign in front of what is typed, and the
// keyboard there on the frequency window's next frame.
void selectAc(const tower::State& s, const TowerActions& a, int id) {
    if (a.select) a.select(id);
    const tower::Aircraft* ac = findAc(s, id);
    if (!ac) return;
    const std::string next = tw::withCallsign(g_ui.cmd, ac->cs);
    if (next != g_ui.cmd) { snprintf(g_ui.cmd, sizeof g_ui.cmd, "%s", next.c_str()); g_ui.cmdReplace = true; }
    g_ui.focusCmd = true;
    g_ui.wantFrequencyFront = true;
}
void putInCommandLine(const std::string& text) {
    snprintf(g_ui.cmd, sizeof g_ui.cmd, "%s", text.c_str());
    g_ui.cmdReplace = true;
    g_ui.focusCmd = true;
    g_ui.wantFrequencyFront = true;
}

// A toggle switch: a pill with a knob, the label after it.
bool toggle(const char* id, const char* label, bool on) {
    const Palette& p = palette();
    const float h = ImGui::GetFrameHeight() * 0.72f, w = h * 1.8f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float y = at.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##t", ImVec2(w + ImGui::GetStyle().ItemInnerSpacing.x + textWidth(nullptr, label), ImGui::GetFrameHeight()));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + h), col(on ? p.ok : (hov ? p.line : p.panelAlt)), h * 0.5f);
    dl->AddCircleFilled(ImVec2(on ? at.x + w - h * 0.5f : at.x + h * 0.5f, y + h * 0.5f), h * 0.5f - 2.f, col(on ? ImVec4(1, 1, 1, 1) : p.muted));
    textAt(dl, nullptr, ImVec2(at.x + w + ImGui::GetStyle().ItemInnerSpacing.x, at.y + ImGui::GetStyle().FramePadding.y), col(p.text), label);
    return clicked;
}

// A spinner for "waiting" states.
void spinner(float r, float now) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 c(p.x + r + 2, p.y + h * 0.5f);
    const float a0 = now * 5.f;
    dl->PathArcTo(c, r, a0, a0 + kPi * 1.4f, 20);
    dl->PathStroke(col(palette().accent), 0, 2.f);
    ImGui::Dummy(ImVec2(r * 2 + 6, h));
}

// ---------------------------------------------------------------- ground radar
struct RadarPal { ImU32 bg, pavement, runway, runwayEdge, marking, rwyText, closed, taxiFill, taxiLine, taxiText, stand, standText, holdLit, holdOff, ring, ringText, towerIcon, labelBg; };
RadarPal radarPal() {
    RadarPal r;
    if (darkTheme()) {
        r.bg = IM_COL32(13, 18, 24, 255); r.pavement = IM_COL32(46, 54, 63, 255); r.runway = IM_COL32(78, 87, 98, 255); r.runwayEdge = IM_COL32(140, 150, 162, 255);
        r.marking = IM_COL32(235, 238, 242, 210); r.rwyText = IM_COL32(225, 230, 236, 235); r.closed = IM_COL32(255, 96, 92, 230); r.labelBg = IM_COL32(13, 18, 24, 170); r.taxiFill = IM_COL32(46, 54, 63, 255); r.taxiLine = IM_COL32(212, 180, 70, 110);
        r.taxiText = IM_COL32(212, 190, 110, 230); r.stand = IM_COL32(120, 136, 152, 255); r.standText = IM_COL32(140, 154, 170, 230);
        r.holdLit = IM_COL32(255, 70, 60, 255); r.holdOff = IM_COL32(140, 70, 66, 200); r.ring = IM_COL32(92, 163, 245, 52); r.ringText = IM_COL32(92, 163, 245, 150);
        r.towerIcon = IM_COL32(220, 226, 235, 255);
    } else {
        r.bg = IM_COL32(222, 228, 233, 255); r.pavement = IM_COL32(176, 184, 192, 255); r.runway = IM_COL32(112, 120, 130, 255); r.runwayEdge = IM_COL32(70, 78, 88, 255);
        r.marking = IM_COL32(255, 255, 255, 235); r.rwyText = IM_COL32(40, 46, 54, 255); r.closed = IM_COL32(210, 40, 36, 240); r.labelBg = IM_COL32(236, 240, 243, 190); r.taxiFill = IM_COL32(176, 184, 192, 255); r.taxiLine = IM_COL32(190, 146, 10, 150);
        r.taxiText = IM_COL32(120, 92, 0, 240); r.stand = IM_COL32(92, 102, 114, 255); r.standText = IM_COL32(70, 80, 92, 230);
        r.holdLit = IM_COL32(215, 30, 26, 255); r.holdOff = IM_COL32(170, 100, 96, 200); r.ring = IM_COL32(40, 115, 217, 60); r.ringText = IM_COL32(40, 115, 217, 170);
        r.towerIcon = IM_COL32(40, 46, 54, 255);
    }
    return r;
}

void drawPlane(ImDrawList* dl, ImVec2 c, float hdgDeg, float sizePx, ImU32 fill, ImU32 outline) {
    const auto& shape = tw::planeShape();
    const float h = hdgDeg * kPi / 180.f, cs = std::cos(h), sn = std::sin(h);
    ImVec2 pts[32];
    int n = 0;
    for (const auto& q : shape) {
        if (n >= 32) break;
        const float x = q.x * sizePx, y = q.y * sizePx;
        const float wx = x * cs + y * sn, wy = -x * sn + y * cs;   // clockwise by the heading, north up
        pts[n++] = ImVec2(c.x + wx, c.y - wy);
    }
    dl->AddConcavePolyFilled(pts, n, fill);
    if (outline) dl->AddPolyline(pts, n, outline, ImDrawFlags_Closed, 1.3f);
}

struct AcOnScreen { const tower::Aircraft* a; ImVec2 pos; float size; ImVec2 l0, l1; int lines = 0; std::string line2; };

// A label's second line: type and state on the ground, level and speed in the air, what a vehicle is.
std::string labelLine2(const tower::Aircraft& ac) {
    char t[64];
    if (ac.veh) snprintf(t, sizeof t, "%s", ac.vtype.empty() ? tr("vehicle") : ac.vtype.c_str());
    else if (!ac.onGround) snprintf(t, sizeof t, "%03d  %.0f kt", (int)std::lround(ac.altFt / 100.f), ac.gsKt);
    else snprintf(t, sizeof t, "%s %s", ac.type.c_str(), tw::shortState(ac).c_str());
    return t;
}

void drawRadar(const TowerFrame& f, const tower::State& s, const TowerActions& a) {
    (void)f;
    TowerUi& u = g_ui;
    const Palette& p = palette();
    const RadarPal rp = radarPal();
    ImGuiIO& io = ImGui::GetIO();
    const tower::Aircraft* sel = findAc(s, s.selectedId);
    // ---- toolbar
    if (ImGui::SmallButton(tr("Fit"))) u.viewFitted = false;
    probeItem("fit");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("the whole airport"));
    ImGui::SameLine();
    if (ImGui::SmallButton(" - ")) u.view.ppm = std::max(0.002f, u.view.ppm / 1.4f);
    ImGui::SameLine();
    if (ImGui::SmallButton(" + ")) u.view.ppm = std::min(4.f, u.view.ppm * 1.4f);
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    const bool following = sel && s.followId == sel->id;
    if (ImGui::SmallButton(following ? tr("Stop following") : tr("Follow"))) { if (a.follow) a.follow(following ? -1 : sel->id); }
    probeItem("follow");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("the camera follows the selected aircraft"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Look at")) && a.lookAt && sel) a.lookAt(sel->id);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 14.f);
    pushFont(g_fonts.small); ImGui::AlignTextToFramePadding(); muted(tr("Binoculars")); popFont(g_fonts.small);
    ImGui::SameLine();
    if (ImGui::SmallButton(" - ##bino") && a.binoculars) a.binoculars(0.8f);
    ImGui::SameLine();
    if (ImGui::SmallButton(" + ##bino") && a.binoculars) a.binoculars(1.25f);
    ImGui::SameLine(0, 14.f);
    bool labels = u.labels;
    if (ImGui::Checkbox(tr("Labels"), &labels)) u.labels = labels;
    // ---- the scope
    const float hintH = smallH() + 4.f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y -= hintH;
    if (size.x < 50.f || size.y < 50.f) return;
    ImGui::InvisibleButton("##scope", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    probeItem("radar");
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    if (!u.viewFitted || u.viewIcao != s.icao) { u.view = tw::fit(s, T(size)); u.viewIcao = s.icao; u.viewFitted = true; }
    tw::View& v = u.view;
    if (hovered && io.MouseWheel != 0.f) tw::zoomAt(v, T(origin), T(size), io.MouseWheel > 0 ? 1.25f : 0.8f, T(io.MousePos));
    if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.f)) tw::pan(v, io.MouseDelta.x, io.MouseDelta.y);
    if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 3.f) && a.look) a.look(io.MouseDelta.x * 0.25f, -io.MouseDelta.y * 0.25f);
    auto S = [&](float x, float y) { return V(tw::toScreen(v, T(origin), T(size), x, y)); };
    const ImVec2 o1(origin.x + size.x, origin.y + size.y);
    auto visible = [&](ImVec2 q, float m) { return q.x > origin.x - m && q.x < o1.x + m && q.y > origin.y - m && q.y < o1.y + m; };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, o1, true);
    dl->AddRectFilled(origin, o1, rp.bg);
    // ---- pavement (filled), holes in the background colour
    std::vector<ImVec2> pts;
    auto ringOnScreen = [&](const tower::Ring& r) {
        pts.clear();
        float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
        for (const auto& q : r) { const ImVec2 sp = S(q.x, q.y); pts.push_back(sp); x0 = std::min(x0, sp.x); y0 = std::min(y0, sp.y); x1 = std::max(x1, sp.x); y1 = std::max(y1, sp.y); }
        return x1 >= origin.x && x0 <= o1.x && y1 >= origin.y && y0 <= o1.y && (x1 - x0) + (y1 - y0) > 1.5f;
    };
    for (const auto& poly : s.pavement) if (!poly.empty() && ringOnScreen(poly[0])) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), rp.pavement);
    for (const auto& poly : s.pavement) for (size_t k = 1; k < poly.size(); ++k) if (ringOnScreen(poly[k])) dl->AddConcavePolyFilled(pts.data(), (int)pts.size(), rp.bg);
    // ---- taxiways: a band of pavement along each edge (rounded where edges meet, so a turn has no notch) --
    // the same colour as the pavement polygons, so it also fills in where an airport's polygons are sparse --
    // and the yellow centre lines on top once they are far enough apart to read
    std::vector<float> joint(s.taxiNodes.size(), 0.f);
    for (const auto& e : s.taxiEdges) {
        const tower::Pt& na = s.taxiNodes[(size_t)e.a]; const tower::Pt& nb = s.taxiNodes[(size_t)e.b];
        const ImVec2 sa = S(na.x, na.y), sb = S(nb.x, nb.y);
        if (!visible(sa, 300.f) && !visible(sb, 300.f)) continue;
        const float w = std::max(1.5f, e.widthM * v.ppm);
        dl->AddLine(sa, sb, rp.taxiFill, w);
        joint[(size_t)e.a] = std::max(joint[(size_t)e.a], w);
        joint[(size_t)e.b] = std::max(joint[(size_t)e.b], w);
    }
    for (size_t i = 0; i < joint.size(); ++i) {
        if (joint[i] < 3.f) continue;
        const ImVec2 q = S(s.taxiNodes[i].x, s.taxiNodes[i].y);
        if (visible(q, joint[i])) dl->AddCircleFilled(q, joint[i] * 0.5f, rp.taxiFill, 16);
    }
    if (v.ppm > 0.12f) {
        for (const auto& e : s.taxiEdges) {
            const tower::Pt& na = s.taxiNodes[(size_t)e.a]; const tower::Pt& nb = s.taxiNodes[(size_t)e.b];
            const ImVec2 sa = S(na.x, na.y), sb = S(nb.x, nb.y);
            if (visible(sa, 300.f) || visible(sb, 300.f)) dl->AddLine(sa, sb, rp.taxiLine, v.ppm > 0.6f ? 1.4f : 1.f);
        }
    }
    // ---- runways: the strip, the edges, a dashed centre line, the designators beyond each end
    for (size_t i = 0; i < s.runways.size(); ++i) {
        const tower::Runway& r = s.runways[i];
        const ImVec2 e0 = S(r.ends[0].x, r.ends[0].y), e1 = S(r.ends[1].x, r.ends[1].y);
        const float dx = e1.x - e0.x, dy = e1.y - e0.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.f) continue;
        const float ux = dx / len, uy = dy / len, hw = std::max(1.5f, r.width * v.ppm * 0.5f);
        const ImVec2 nrm(-uy * hw, ux * hw);
        const ImVec2 c0(e0.x + nrm.x, e0.y + nrm.y), c1(e1.x + nrm.x, e1.y + nrm.y), c2(e1.x - nrm.x, e1.y - nrm.y), c3(e0.x - nrm.x, e0.y - nrm.y);
        dl->AddQuadFilled(c0, c1, c2, c3, rp.runway);
        if (hw > 3.f) { dl->AddLine(c0, c1, rp.runwayEdge, 1.f); dl->AddLine(c3, c2, rp.runwayEdge, 1.f); }
        if (hw > 4.f) {
            const float dash = std::max(6.f, 30.f * v.ppm), gap = dash * 0.8f;
            for (float t = hw * 2.f; t + dash < len - hw * 2.f; t += dash + gap)
                dl->AddLine(ImVec2(e0.x + ux * t, e0.y + uy * t), ImVec2(e0.x + ux * (t + dash), e0.y + uy * (t + dash)), rp.marking, 1.f);
            dl->AddLine(c0, c3, rp.marking, 2.f);
            dl->AddLine(c1, c2, rp.marking, 2.f);
        }
        const bool closed = std::find(s.closedRunways.begin(), s.closedRunways.end(), (int)i) != s.closedRunways.end();
        if (closed) {
            const ImVec2 m((e0.x + e1.x) * 0.5f, (e0.y + e1.y) * 0.5f);
            const float k = std::max(6.f, hw * 1.6f);
            dl->AddLine(ImVec2(m.x - k, m.y - k), ImVec2(m.x + k, m.y + k), rp.closed, 3.f);
            dl->AddLine(ImVec2(m.x - k, m.y + k), ImVec2(m.x + k, m.y - k), rp.closed, 3.f);
        }
        for (int k = 0; k < 2; ++k) {
            const ImVec2 e = k == 0 ? e0 : e1;
            const float sgn = k == 0 ? -1.f : 1.f;
            const char* name = r.ends[(size_t)k].name.c_str();
            const float tw = textWidth(g_fonts.small, name);
            const ImVec2 at(e.x + ux * sgn * (14.f + tw * 0.5f) - tw * 0.5f, e.y + uy * sgn * (14.f + smallH() * 0.5f) - smallH() * 0.5f);
            const bool inUse = r.ends[(size_t)k].name == s.rwy;
            textAt(dl, g_fonts.small, at, inUse ? col(p.ok) : rp.rwyText, name);
        }
    }
    // ---- taxiway names, one per name per neighbourhood, once the picture is close enough to read them
    if (v.ppm > 0.32f) {
        std::vector<std::pair<std::string, ImVec2>> placed;
        for (const auto& e : s.taxiEdges) {
            if (e.name.empty()) continue;
            const tower::Pt& na = s.taxiNodes[(size_t)e.a]; const tower::Pt& nb = s.taxiNodes[(size_t)e.b];
            const ImVec2 sa = S(na.x, na.y), sb = S(nb.x, nb.y), m((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f);
            if (!visible(m, -10.f)) continue;
            const float tw = textWidth(g_fonts.small, e.name.c_str());
            const float segLen = std::sqrt((sb.x - sa.x) * (sb.x - sa.x) + (sb.y - sa.y) * (sb.y - sa.y));
            if (segLen < tw + 12.f) continue;
            bool near = false;
            for (const auto& pl : placed) {
                const float ddx = pl.second.x - m.x, ddy = pl.second.y - m.y;
                if ((pl.first == e.name && ddx * ddx + ddy * ddy < 160.f * 160.f) || ddx * ddx + ddy * ddy < 26.f * 26.f) { near = true; break; }
            }
            if (near) continue;
            placed.emplace_back(e.name, m);
            const ImVec2 t0(m.x - tw * 0.5f - 3.f, m.y - smallH() * 0.5f - 1.f), t1(m.x + tw * 0.5f + 3.f, m.y + smallH() * 0.5f + 1.f);
            dl->AddRectFilled(t0, t1, rp.bg & 0xC0FFFFFF, 3.f);
            textAt(dl, g_fonts.small, ImVec2(t0.x + 3.f, t0.y + 1.f), rp.taxiText, e.name.c_str());
        }
    }
    // ---- stands and stop bars
    if (u.stands && v.ppm > 0.22f) {
        for (const auto& st : s.stands) {
            const ImVec2 q = S(st.x, st.y);
            if (!visible(q, 20.f)) continue;
            dl->AddCircleFilled(q, v.ppm > 0.6f ? 3.f : 2.f, rp.stand);
            if (v.ppm > 0.55f) textAt(dl, g_fonts.small, ImVec2(q.x + 4.f, q.y - smallH() * 0.5f), rp.standText, st.name.c_str());
        }
    }
    const bool barsLit = s.haveStopBars ? s.stopBars : s.lvp;
    for (const auto& h : s.holdLines) {
        const ImVec2 q = S(h.x, h.y);
        if (!visible(q, 10.f)) continue;
        dl->AddRectFilled(ImVec2(q.x - 3.f, q.y - 1.5f), ImVec2(q.x + 3.f, q.y + 1.5f), barsLit ? rp.holdLit : rp.holdOff, 1.f);
    }
    // ---- range rings round the tower (or the airport's datum)
    const ImVec2 centre = s.haveTower ? S(s.towerX, s.towerY) : S(0.f, 0.f);
    const float step = tw::ringStep(v.ppm);
    const float far = std::sqrt(size.x * size.x + size.y * size.y) + std::fabs(centre.x - (origin.x + size.x * 0.5f)) + std::fabs(centre.y - (origin.y + size.y * 0.5f));
    const float ringFade = std::max(0.25f, std::min(1.f, (0.9f - v.ppm) / 0.6f));   // close up on an apron they would only be clutter
    for (int k = 1; k <= 12 && k * step * v.ppm < far; ++k) {
        const float r = k * step * v.ppm;
        dl->AddCircle(centre, r, fade(rp.ring, ringFade), 96, 1.f);
        const std::string t = tw::rangeText(k * step);
        const ImVec2 at(centre.x + 3.f, centre.y - r - smallH() - 1.f);
        if (visible(at, -2.f)) textAt(dl, g_fonts.small, at, fade(rp.ringText, ringFade), t.c_str());
    }
    if (s.haveTower) {
        dl->AddRectFilled(ImVec2(centre.x - 4.f, centre.y - 4.f), ImVec2(centre.x + 4.f, centre.y + 4.f), rp.towerIcon, 1.f);
        if (v.ppm > 0.08f) textAt(dl, g_fonts.small, ImVec2(centre.x + 7.f, centre.y - smallH() * 0.5f), rp.towerIcon, "TWR");
    }
    // ---- aircraft: where they are on the screen, then a click, then the drawing on top of everything
    std::vector<AcOnScreen> onScreen;
    const float lh = ImGui::GetTextLineHeight();
    for (const auto& ac : s.ac) {
        const ImVec2 q = S(ac.x, ac.y);
        if (!visible(q, 60.f)) continue;
        AcOnScreen o;
        o.a = &ac; o.pos = q;
        o.size = ac.veh ? std::max(7.f, 7.f * v.ppm) : std::max(ac.onGround ? 15.f : 17.f, tw::wingspanM(ac.wtc) * v.ppm);
        onScreen.push_back(o);
        probeRect("ac:" + ac.cs, ImVec2(q.x - o.size * 0.5f, q.y - o.size * 0.5f), ImVec2(q.x + o.size * 0.5f, q.y + o.size * 0.5f));
    }
    // Labels where they cover nothing -- TowerTrainer's own layout. The most important are placed first (the
    // selected aircraft, anything calling or in trouble, airborne traffic, aircraft moving, parked ones, vehicles);
    // each keeps last frame's place while that is free, else takes the first free one of eight round its symbol,
    // then the same further out on a longer leader line. A parked aircraft or a vehicle that finds no room keeps
    // quiet; anything else drops to its callsign, and as a last resort covers the least it can.
    {
        auto callingNow = [&](const tower::Aircraft& ac) { return ac.callingUntilMs > s.utcMs; };
        std::vector<std::pair<ImVec2, ImVec2>> taken;   // the symbols (in onScreen's order), then labels as placed
        for (const auto& o : onScreen) taken.push_back({ImVec2(o.pos.x - o.size * 0.5f, o.pos.y - o.size * 0.5f), ImVec2(o.pos.x + o.size * 0.5f, o.pos.y + o.size * 0.5f)});
        std::vector<size_t> order(onScreen.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t i, size_t j) {
            const tower::Aircraft& x = *onScreen[i].a; const tower::Aircraft& y = *onScreen[j].a;
            return tw::labelPriority(x, x.id == s.selectedId, callingNow(x)) > tw::labelPriority(y, y.id == s.selectedId, callingNow(y));
        });
        auto overlap = [](ImVec2 a0, ImVec2 a1, ImVec2 b0, ImVec2 b1) {
            const float w = std::min(a1.x, b1.x) - std::max(a0.x, b0.x), h = std::min(a1.y, b1.y) - std::max(a0.y, b0.y);
            return w > 0.f && h > 0.f ? w * h : 0.f;
        };
        std::map<int, int> slots;
        for (size_t idx : order) {
            AcOnScreen& o = onScreen[idx];
            const tower::Aircraft& ac = *o.a;
            const bool isSel = ac.id == s.selectedId, calling = callingNow(ac);
            o.lines = tw::labelLines(ac, isSel, ac.id == u.hoverId, calling, v.ppm, u.labels);
            if (o.lines == 0) continue;
            o.line2 = labelLine2(ac);
            const float wCs = textWidth(nullptr, ac.cs.c_str()), w2 = textWidth(g_fonts.small, o.line2.c_str());
            // slot = ring * 8 + place: eight places round the symbol (up-right first), three rings further out
            auto box = [&](int slot, int lines, ImVec2* b0, ImVec2* b1) {
                static const float extra[3] = {0.f, 14.f, 30.f};
                const float w = std::max(wCs, lines > 1 ? w2 : 0.f) + 6.f, h = lh + (lines > 1 ? smallH() : 0.f) + 2.f;
                const float r = o.size * 0.5f + 5.f + extra[(slot / 8) % 3];
                const ImVec2 c[8] = {{r, -r - h}, {r, r}, {-r - w, -r - h}, {-r - w, r}, {r + 3.f, -h * 0.5f}, {-r - w - 3.f, -h * 0.5f}, {-w * 0.5f, -r - h - 3.f}, {-w * 0.5f, r + 3.f}};
                *b0 = ImVec2(o.pos.x + c[slot % 8].x, o.pos.y + c[slot % 8].y);
                *b1 = ImVec2(b0->x + w, b0->y + h);
            };
            // what a place costs: the area it covers, and more; `any` stops at the first thing (is it free at all?)
            auto cost = [&](int slot, int lines, bool any = false) {
                ImVec2 b0, b1;
                box(slot, lines, &b0, &b1);
                float c = 0.f;
                if (b0.x < origin.x || b1.x > o1.x || b0.y < origin.y || b1.y > o1.y) {   // off the scope
                    c += (b1.x - b0.x) * (b1.y - b0.y) * 0.5f + 1.f;
                    if (any) return c;
                }
                for (size_t k = 0; k < taken.size(); ++k) {   // not touching either: 2 px round each label
                    if (k != idx) c += overlap(ImVec2(b0.x - 2.f, b0.y - 1.f), ImVec2(b1.x + 2.f, b1.y + 1.f), taken[k].first, taken[k].second);
                    if (any && c > 0.f) return c;
                }
                // a label nearer another target than its own reads as that one's: not free, though better than covering
                const float mx = (b0.x + b1.x) * 0.5f - o.pos.x, my = (b0.y + b1.y) * 0.5f - o.pos.y, own = mx * mx + my * my;
                for (size_t k = 0; k < onScreen.size(); ++k) {
                    if (k == idx) continue;
                    const float kx = (b0.x + b1.x) * 0.5f - onScreen[k].pos.x, ky = (b0.y + b1.y) * 0.5f - onScreen[k].pos.y;
                    if (kx * kx + ky * ky < own) { c += 40.f; break; }
                }
                return c;
            };
            const auto prev = u.labelSlot.find(ac.id);
            auto freeSlot = [&](int lines) {
                if (prev != u.labelSlot.end() && cost(prev->second, lines, true) == 0.f) return prev->second;
                for (int sl = 0; sl < 24; ++sl) if (cost(sl, lines, true) == 0.f) return sl;
                return -1;
            };
            int slot = freeSlot(o.lines);
            if (slot < 0 && tw::labelMayHide(ac, isSel, calling)) { o.lines = 0; continue; }
            if (slot < 0 && o.lines > 1 && !isSel && (slot = freeSlot(1)) >= 0) o.lines = 1;
            if (slot < 0) {
                float best = FLT_MAX;
                for (int sl = 0; sl < 8; ++sl) { const float c = cost(sl, o.lines); if (c < best) { best = c; slot = sl; } }
            }
            box(slot, o.lines, &o.l0, &o.l1);
            taken.push_back({o.l0, o.l1});
            slots[ac.id] = slot;
        }
        u.labelSlot.swap(slots);
    }
    auto hit = [&](ImVec2 m) -> int {
        int best = -1;
        float bd = 1e9f;
        for (const auto& o : onScreen) {
            const float dx = m.x - o.pos.x, dy = m.y - o.pos.y, d = dx * dx + dy * dy, r = std::max(10.f, o.size * 0.6f);
            const bool onLabel = o.lines > 0 && m.x >= o.l0.x && m.x <= o.l1.x && m.y >= o.l0.y && m.y <= o.l1.y;
            if ((d <= r * r || onLabel) && d < bd) { bd = d; best = o.a->id; }
        }
        return best;
    };
    u.hoverId = hovered && !held ? hit(io.MousePos) : -1;   // its whole label from the next frame
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 16.f) {
        const int id = hit(io.MousePos);
        if (id >= 0) selectAc(s, a, id); else if (a.select) a.select(-1);
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[1] < 16.f) {
        u.contextId = hit(io.MousePos);
        if (u.contextId >= 0) ImGui::OpenPopup("##acmenu");
    }
    for (const auto& o : onScreen) {
        const tower::Aircraft& ac = *o.a;
        const ImVec4 kc = kindColour(ac);
        const bool isSel = ac.id == s.selectedId;
        const ImU32 fill = col(kc), outline = isSel ? col(darkTheme() ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1)) : col(ImVec4(0, 0, 0, 0.55f));
        if (ac.veh) dl->AddRectFilled(ImVec2(o.pos.x - o.size * 0.5f, o.pos.y - o.size * 0.5f), ImVec2(o.pos.x + o.size * 0.5f, o.pos.y + o.size * 0.5f), fill, 2.f);
        else drawPlane(dl, o.pos, ac.hdg, o.size, fill, outline);
        if (isSel) dl->AddCircle(o.pos, o.size * 0.62f + 5.f, col(p.accent), 32, 2.f);
        if (ac.handoffIn && blinkOn(s.now)) dl->AddCircle(o.pos, o.size * 0.62f + 8.f, col(p.warn), 32, 2.f);
        if (o.lines == 0) {   // decluttered: the symbol alone
            if (s.followId == ac.id) textAt(dl, g_fonts.small, ImVec2(o.pos.x + o.size * 0.5f + 3.f, o.pos.y - smallH() * 0.5f), col(p.ok), tr("followed"));
            continue;
        }
        // the label: callsign, then type / altitude and speed, joined to the symbol by a leader line from its edge
        // to the nearest corner of the label
        const ImVec2 corner(o.l0.x > o.pos.x ? o.l0.x : o.l1.x, o.l0.y > o.pos.y ? o.l0.y : (o.l1.y < o.pos.y ? o.l1.y : (o.l0.y + o.l1.y) * 0.5f));
        const float ddx = corner.x - o.pos.x, ddy = corner.y - o.pos.y, dd = std::sqrt(ddx * ddx + ddy * ddy);
        if (dd > o.size * 0.5f + 2.f) dl->AddLine(ImVec2(o.pos.x + ddx / dd * o.size * 0.5f, o.pos.y + ddy / dd * o.size * 0.5f), corner, col(kc, 0.6f), 1.f);
        dl->AddRectFilled(o.l0, o.l1, isSel ? col(p.panel, 0.92f) : rp.labelBg, 3.f);   // readable over a runway too
        probeRect("label:" + ac.cs, o.l0, o.l1);
        textAt(dl, nullptr, ImVec2(o.l0.x + 3.f, o.l0.y + 1.f), col(isSel ? p.text : kc), ac.cs.c_str());
        if (o.lines > 1) textAt(dl, g_fonts.small, ImVec2(o.l0.x + 3.f, o.l0.y + 1.f + lh), col(p.muted), o.line2.c_str());
        if (s.followId == ac.id) textAt(dl, g_fonts.small, ImVec2(o.l1.x + 3.f, o.l0.y + 2.f), col(p.ok), tr("followed"));
    }
    // the scale, bottom left
    {
        const float px = step * v.ppm;
        const ImVec2 b0(origin.x + 10.f, o1.y - 10.f);
        dl->AddLine(b0, ImVec2(b0.x + px, b0.y), rp.ringText, 2.f);
        dl->AddLine(b0, ImVec2(b0.x, b0.y - 4.f), rp.ringText, 2.f);
        dl->AddLine(ImVec2(b0.x + px, b0.y), ImVec2(b0.x + px, b0.y - 4.f), rp.ringText, 2.f);
        textAt(dl, g_fonts.small, ImVec2(b0.x + 4.f, b0.y - 5.f - smallH()), rp.ringText, tw::rangeText(step).c_str());
    }
    dl->PopClipRect();
    if (ImGui::BeginPopup("##acmenu")) {
        const tower::Aircraft* c = findAc(s, u.contextId);
        if (!c) ImGui::CloseCurrentPopup();
        else {
            ImGui::TextColored(kindColour(*c), "%s", c->cs.c_str());
            ImGui::Separator();
            const bool pick = ImGui::MenuItem(tr("Select"));
            probeItem("menu:select");
            const bool fol = ImGui::MenuItem(s.followId == c->id ? tr("Stop following") : tr("Follow with the camera"));
            probeItem("menu:follow");
            const bool look = ImGui::MenuItem(tr("Look at it"));
            probeItem("menu:look");
            const bool acc = c->handoffIn && ImGui::MenuItem(tr("Accept the handoff"));
            if (c->handoffIn) probeItem("menu:accept");
            if (pick) selectAc(s, a, c->id);
            if (fol && a.follow) a.follow(s.followId == c->id ? -1 : c->id);
            if (look && a.lookAt) a.lookAt(c->id);
            if (acc && a.accept) a.accept(c->id);
        }
        ImGui::EndPopup();
    }
    // ---- the hint line
    pushFont(g_fonts.small);
    char info[160];
    snprintf(info, sizeof info, "%s  %s%s  %d %s   ", s.icao.c_str(), s.rwy.empty() ? "" : "RWY ", s.rwy.c_str(), (int)s.ac.size(), tr("aircraft"));
    ImGui::TextColored(p.text, "%s", info);
    ImGui::SameLine(0, 0);
    muted(tr("wheel: zoom   drag: move   right-drag: look around   click: select   right-click: more"));
    popFont(g_fonts.small);
}

// ---------------------------------------------------------------- strips
void stripCard(const tower::State& s, const tower::Aircraft& ac, const TowerActions& a, bool incoming, float width) {
    const Palette& p = palette();
    const float lh = ImGui::GetTextLineHeight(), h = lh + smallH() + 12.f;
    ImGui::PushID(ac.id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width, p0.y + h);
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(width, h));
    const bool hov = ImGui::IsItemHovered();
    probeItem("strip:" + ac.cs);
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        ImGui::SetDragDropPayload("XR_TOWER_STRIP", &ac.id, sizeof(int));
        ImGui::TextColored(kindColour(ac), "%s", ac.cs.c_str());
        ImGui::SameLine(); muted(tr("to another bay"));
        ImGui::EndDragDropSource();
    }
    if (clicked) selectAc(s, a, ac.id);
    const ImVec2 after = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 kc = kindColour(ac);
    const bool isSel = ac.id == s.selectedId;
    const bool calling = ac.callingUntilMs > 0 && s.utcMs > 0 && ac.callingUntilMs > s.utcMs;
    dl->AddRectFilled(p0, p1, col(hov ? mix(p.panelAlt, p.accent, 0.12f) : p.panelAlt), 4.f);
    dl->AddRectFilled(p0, ImVec2(p0.x + 5.f, p1.y), col(kc), 4.f, ImDrawFlags_RoundCornersLeft);
    if (isSel) dl->AddRect(p0, p1, col(p.accent), 4.f, 0, 2.f);
    else if (incoming && blinkOn(s.now)) dl->AddRect(p0, p1, col(p.warn), 4.f, 0, 2.f);
    // line one: callsign, type / wake, and on the right the squawk and the state
    float x = p0.x + 12.f;
    const float y = p0.y + 5.f;
    textAt(dl, nullptr, ImVec2(x, y), col(p.text), ac.cs.c_str());
    x += textWidth(nullptr, ac.cs.c_str()) + 10.f;
    char tw_[32]; snprintf(tw_, sizeof tw_, "%s%s%s", ac.type.c_str(), ac.wtc.empty() ? "" : "/", ac.wtc.c_str());
    textAt(dl, nullptr, ImVec2(x, y), col(p.muted), tw_);
    if (calling && blinkOn(s.now, 2.f)) dl->AddCircleFilled(ImVec2(x + textWidth(nullptr, tw_) + 10.f, y + lh * 0.5f), 4.f, col(p.ok));
    const std::string st = tw::shortState(ac);
    const float rightEdge = p1.x - (incoming ? 80.f : 10.f);
    const float stW = textWidth(g_fonts.small, st.c_str()) + 10.f;
    dl->AddRectFilled(ImVec2(rightEdge - stW, y + 1.f), ImVec2(rightEdge, y + lh - 1.f), col(kc, 0.22f), 3.f);
    textAt(dl, g_fonts.small, ImVec2(rightEdge - stW + 5.f, y + (lh - smallH()) * 0.5f), col(kc), st.c_str());
    if (!ac.veh) {
        const std::string sq = !ac.assignedSq.empty() ? ac.assignedSq : ac.squawk;
        const bool wrong = !ac.assignedSq.empty() && ac.squawk != ac.assignedSq;
        const float sqW = textWidth(nullptr, sq.c_str());
        textAt(dl, nullptr, ImVec2(rightEdge - stW - 10.f - sqW, y), col(wrong ? p.warn : p.text), sq.c_str());
    }
    // line two: where it goes, stand, runway, SID, cleared level, warnings, a handoff
    std::string l2;
    if (ac.kind == "dep") l2 = (ac.other == "VFR" ? "VFR " + ac.otherName : "\xE2\x86\x92 " + ac.other + (ac.otherName.empty() ? "" : " " + ac.otherName));
    else if (ac.kind == "arr") l2 = ac.other + (ac.otherName.empty() ? "" : " " + ac.otherName) + " \xE2\x86\x92";
    else if (ac.veh) l2 = ac.vtype;
    if (!ac.stand.empty()) l2 += "   " + std::string(tr("stand")) + " " + ac.stand;
    if (!ac.rwy.empty() && !ac.veh) l2 += "   RWY " + ac.rwy;
    if (!ac.sid.empty()) l2 += "   " + ac.sid;
    if (ac.cfl > 0) l2 += "   CFL " + tw::levelText(std::to_string(ac.cfl));
    textAt(dl, g_fonts.small, ImVec2(p0.x + 12.f, y + lh + 3.f), col(p.muted), l2.c_str());
    float wx = p0.x + 12.f + textWidth(g_fonts.small, l2.c_str()) + 10.f;
    auto chip = [&](const std::string& t, const ImVec4& c) {
        const float w = textWidth(g_fonts.small, t.c_str()) + 8.f;
        if (wx + w > rightEdge) return;
        dl->AddRectFilled(ImVec2(wx, y + lh + 2.f), ImVec2(wx + w, y + lh + 3.f + smallH()), col(c, 0.25f), 3.f);
        textAt(dl, g_fonts.small, ImVec2(wx + 4.f, y + lh + 3.f), col(c), t.c_str());
        wx += w + 4.f;
    };
    for (const auto& w : ac.warn) chip(w, w.rfind("SQK", 0) == 0 ? p.warn : p.err);
    if (ac.waitingS > 45) chip("WAIT " + tw::waitingText(ac.waitingS), ac.waitingS > 120 ? p.err : p.warn);
    if (!ac.handoffFrom.empty() && incoming) chip("\xE2\x86\x90 " + ac.handoffFrom, p.warn);   // handed over by
    if (incoming) {
        const float bw = 70.f;
        ImGui::SetCursorScreenPos(ImVec2(p1.x - bw - 6.f, p0.y + (h - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, col(p.warn, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(p.warn, 0.6f));
        if (ImGui::Button(tr("Accept"), ImVec2(bw, 0)) && a.accept) a.accept(ac.id);
        ImGui::PopStyleColor(2);
        probeItem("accept:" + ac.cs);
    }
    // back under the card -- with an empty item there, as ImGui wants a moved cursor confirmed by one
    ImGui::SetCursorScreenPos(after);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::Dummy(ImVec2(0.f, 0.f));
    ImGui::PopStyleVar();
    ImGui::PopID();
}

void drawStrips(const TowerFrame& f, const tower::State& s, const TowerActions& a) {
    const Palette& p = palette();
    const std::string pos = myPos(s, f);
    const auto it = s.positions.find(pos);
    heading((pos + " " + std::string(tr("strips"))).c_str());
    if (it != s.positions.end() && !it->second.freq.empty()) { ImGui::SameLine(); muted((it->second.name + "  " + it->second.freq).c_str()); }
    ImGui::BeginChild("##bays", ImVec2(0, 0));
    const float width = ImGui::GetContentRegionAvail().x - 2.f;
    const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
    const bool stripDrag = dragging && dragging->IsDataType("XR_TOWER_STRIP");
    for (const auto& bay : tw::baysFor(pos, s.split)) {
        std::vector<const tower::Aircraft*> list;
        for (const auto& ac : s.ac) {
            if (!tw::onBoard(ac, pos, s.split)) continue;
            const bool inc = tw::incomingFor(ac, pos);
            if (bay == "incoming" ? inc : (!inc && tw::bayOf(ac) == bay)) list.push_back(&ac);
        }
        if (bay == "incoming" && list.empty()) continue;
        ImGui::PushID(bay.c_str());
        const ImVec2 g0 = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        pushFont(g_fonts.small);
        ImGui::TextColored(bay == "incoming" ? p.warn : p.muted, "%s", tr(tw::bayTitle(bay)));
        if (!list.empty()) { ImGui::SameLine(); ImGui::TextColored(p.muted, "%d", (int)list.size()); }
        popFont(g_fonts.small);
        if (list.empty()) ImGui::Dummy(ImVec2(width, 6.f));
        for (const auto* ac : list) stripCard(s, *ac, a, bay == "incoming", width);
        ImGui::EndGroup();
        const ImVec2 g1(g0.x + width, ImGui::GetItemRectMax().y);
        probeRect("bay:" + bay, g0, g1);
        if (bay != "incoming" && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("XR_TOWER_STRIP")) {
                const int id = *(const int*)pl->Data;
                if (a.moveStrip) a.moveStrip(id, bay);
            }
            ImGui::EndDragDropTarget();
        }
        if (stripDrag && bay != "incoming" && ImGui::IsMouseHoveringRect(g0, g1))
            ImGui::GetWindowDrawList()->AddRect(ImVec2(g0.x - 2.f, g0.y - 2.f), ImVec2(g1.x + 2.f, g1.y + 2.f), col(p.accent), 4.f, 0, 2.f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(g0.x, g1.y + 3.f), ImVec2(g1.x, g1.y + 3.f), col(p.line, 0.6f));
        ImGui::Dummy(ImVec2(0, 4.f));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- Delivery
void drawDelivery(const TowerFrame& f, const tower::State& s, const TowerActions& a) {
    (void)f;
    const Palette& p = palette();
    int pending = 0;
    for (const auto& r : s.delivery) if (!r.cleared) ++pending;
    heading(tr("Clearance requests"));
    ImGui::SameLine();
    if (pending) ImGui::TextColored(p.warn, "%d %s", pending, tr("waiting")); else muted(tr("none waiting"));
    if (s.delivery.empty()) { ImGui::Spacing(); muted(tr("Nobody has asked for a clearance.")); return; }
    if (ImGui::BeginTable("##del", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Callsign"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn(tr("Type"), ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn(tr("To"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(tr("Stand"), ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn(tr("Waiting"), ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("SID", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("CFL", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(tr("Squawk"), ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableHeadersRow();
        for (const auto& r : s.delivery) {
            ImGui::TableNextRow();
            ImGui::PushID(r.id);
            ImGui::TableNextColumn();
            const tower::Aircraft* ac = findAc(s, r.id);
            const ImVec4 kc = ac ? kindColour(*ac) : p.text;
            ImGui::PushStyleColor(ImGuiCol_Text, r.cleared ? p.muted : kc);
            if (ImGui::Selectable(r.cs.c_str(), r.id == s.selectedId, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) selectAc(s, a, r.id);
            ImGui::PopStyleColor();
            probeItem("del:" + r.cs);
            if (r.live) { ImGui::SameLine(); badge(tr("live"), kindColour(ac ? *ac : tower::Aircraft())); }
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", r.type.c_str(), r.wtc.empty() ? "" : "/", r.wtc.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.dest.c_str());
            if (!r.destName.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.destName.c_str());
            ImGui::TableNextColumn(); if (r.stand.empty()) muted("-"); else ImGui::TextUnformatted(r.stand.c_str());
            ImGui::TableNextColumn();
            if (r.cleared) muted(tr("cleared"));
            else ImGui::TextColored(r.waitingS > 120 ? p.err : r.waitingS > 45 ? p.warn : p.text, "%s", tw::waitingText(r.waitingS).c_str());
            ImGui::TableNextColumn();
            if (r.sid.empty()) muted(tr("vectors")); else ImGui::TextUnformatted(r.sid.c_str());
            if (!r.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.note.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tw::levelText(r.cfl).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.squawk.c_str());
            ImGui::TableNextColumn();
            if (!r.cleared) {
                if (ImGui::SmallButton(tr("Clear")) && !r.clearance.empty()) {
                    if (a.select) a.select(r.id);
                    putInCommandLine(r.clearance);
                }
                probeItem("clear:" + r.cs);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.clearance.c_str());
            } else badge(tr("cleared"), p.ok);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------- the frequency
int cmdCallback(ImGuiInputTextCallbackData* d) {
    TowerUi& u = *(TowerUi*)d->UserData;
    auto replace = [d](const std::string& t) { d->DeleteChars(0, d->BufTextLen); d->InsertChars(0, t.c_str()); d->CursorPos = d->BufTextLen; d->SelectionStart = d->SelectionEnd = d->CursorPos; };
    if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (u.cmdReplace) { replace(u.cmd); u.cmdReplace = false; }
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        std::string out;
        const bool ok = d->EventKey == ImGuiKey_UpArrow ? u.history.up(d->Buf, &out) : u.history.down(&out);
        if (ok) replace(out);
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion && g_completionList) {
        const std::string c = tw::completeCallsign(d->Buf, *g_completionList);
        if (c != d->Buf) replace(c);
    }
    return 0;
}

void logLine(const char* time, const std::string& from, const ImVec4& fromCol, const std::string& text, const ImVec4& textCol, bool guard, const char* tag, const ImVec4& tagCol) {
    const Palette& p = palette();
    if (time && *time) { pushFont(g_fonts.small); ImGui::TextColored(p.muted, "%s", time); popFont(g_fonts.small); ImGui::SameLine(); }
    if (guard) { badge("121.5", p.err); ImGui::SameLine(); }
    if (tag) { badge(tag, tagCol); ImGui::SameLine(); }
    if (!from.empty()) { ImGui::TextColored(fromCol, "%s", from.c_str()); ImGui::SameLine(); }
    ImGui::PushStyleColor(ImGuiCol_Text, textCol);
    ImGui::TextWrapped("%s", tw::displayText(text).c_str());
    ImGui::PopStyleColor();
}

void drawFrequency(const TowerFrame& f, const tower::State& s, const TowerActions& a) {
    TowerUi& u = g_ui;
    const Palette& p = palette();
    const std::string pos = myPos(s, f);
    const auto it = s.positions.find(pos);
    // who is on the air: me (TX), or the latest pilot call (RX)
    const tower::LiveCall* rx = nullptr;
    for (auto r = s.live.rbegin(); r != s.live.rend(); ++r) if (!r->mine && s.now - r->at < 3.f) { rx = &*r; break; }
    light(s.pttDown ? p.tx : (rx ? p.rx : p.muted), s.pttDown || rx != nullptr, 7.f);
    ImGui::SameLine();
    heading((pos + (it != s.positions.end() && !it->second.freq.empty() ? "  " + it->second.freq : std::string())).c_str());
    if (it != s.positions.end() && !it->second.name.empty()) { ImGui::SameLine(); muted(it->second.name.c_str()); }
    ImGui::SameLine();
    if (s.pttDown) ImGui::TextColored(p.tx, "%s", tr("TX - you are transmitting"));
    else if (rx) ImGui::TextColored(p.rx, "RX %s", rx->from.c_str());
    // the log, newest at the bottom
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.f + smallH() + 6.f;
    ImGui::BeginChild("##log", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    for (const auto& e : s.log) {
        const tw::Sender k = tw::senderOf(e);
        const ImVec4 tc = k == tw::Sender::System ? p.muted : k == tw::Sender::Problem ? (e.cls == "err" ? p.err : p.warn) : p.text;
        logLine(e.time.c_str(), e.from, senderColour(k), e.text, tc, e.guard, nullptr, p.text);
    }
    for (const auto& c : s.live) {
        if (s.now - c.at > 25.f || tw::inLog(c, s.log)) continue;
        logLine("", c.mine ? std::string(tr("me")) : c.from, c.atc ? p.atc : p.text, c.text, mix(p.text, p.muted, 0.35f), false, tr("live"), p.muted);
    }
    for (const auto& al : s.alerts) if (s.now - al.second < 20.f) logLine("", "", p.warn, "! " + al.first, p.warn, false, nullptr, p.text);
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    // callsigns the first word could be
    const auto matches = tw::callsignMatches(u.cmd, s.ac, 6);
    if (!matches.empty()) {
        for (size_t i = 0; i < matches.size(); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(matches[i].c_str())) {
                for (const auto& ac : s.ac) if (ac.cs == matches[i]) { if (a.select) a.select(ac.id); break; }
                putInCommandLine(matches[i] + " ");
            }
            probeItem("chip:" + matches[i]);
        }
    } else {
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
    }
    // the command line
    g_completionList = &s.ac;
    if (u.focusCmd) { ImGui::SetKeyboardFocusHere(); u.focusCmd = false; }
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize(tr("Send")).x - ImGui::GetStyle().FramePadding.x * 2.f - ImGui::GetStyle().ItemSpacing.x);
    // Esc clears the line (a second Esc lets go of the keyboard), rather than ImGui's default of putting back the old text
    const ImGuiInputTextFlags fl = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion;
    const bool enter = ImGui::InputTextWithHint("##cmd", tr("Instruction, e.g. FIN321 runway 26 cleared for takeoff"), u.cmd, sizeof u.cmd, fl, cmdCallback, &u);
    probeItem("cmd");
    // A field that is not being edited shows cmd as it is; only an active one has a copy to replace. A flag left
    // over would replace the copy on the next activation and swallow the first letter typed.
    if (!ImGui::IsItemActive()) u.cmdReplace = false;
    ImGui::SameLine();
    const bool send = ImGui::Button(tr("Send"));
    probeItem("send");
    if ((enter || send) && u.cmd[0]) {
        u.history.push(u.cmd);
        if (a.transmit) a.transmit(u.cmd);
        u.cmd[0] = 0;                         // Enter (or the button) ended the edit: the field reads this afresh
        u.focusCmd = true;
    }
    pushFont(g_fonts.small);
    muted(tr("Enter sends   Up / Down: earlier instructions   Tab: completes the callsign   Esc: clears"));
    popFont(g_fonts.small);
}

// ---------------------------------------------------------------- ATIS / operations
void lightButton(const tw::Signal& sg, const tower::State& s, const tower::Aircraft* sel, const TowerActions& a, float w) {
    ImVec4 c(sg.r, sg.g, sg.b, 1.f);
    if (sg.flashing && sel) c.w = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(s.now * 7.f));
    const bool light = sg.r + sg.g + sg.b > 2.2f;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.x, c.y, c.z, 0.85f * c.w));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x, c.y, c.z, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.x * 0.8f, c.y * 0.8f, c.z * 0.8f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text, light || sg.g > 0.8f ? ImVec4(0.05f, 0.07f, 0.09f, 1.f) : ImVec4(1, 1, 1, 1));
    if (ImGui::Button(tr(sg.label), ImVec2(w, ImGui::GetFrameHeight() * 1.25f)) && sel && a.lightGun) a.lightGun(sel->id, sg.id);
    ImGui::PopStyleColor(4);
    probeItem(std::string("light:") + sg.id);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s: %s\n%s: %s", tr("on the ground"), tr(tw::signalMeaning(sg.id, true)), tr("in the air"), tr(tw::signalMeaning(sg.id, false)));
}

void drawOps(const TowerFrame& f, const tower::State& s, const TowerActions& a) {
    (void)f;
    const Palette& p = palette();
    heading((s.icao + "  " + std::string(tr("operations"))).c_str());
    if (!s.hostNick.empty()) { ImGui::SameLine(); muted((std::string(tr("host")) + " " + s.hostNick).c_str()); }
    // runway in use
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Runway in use"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    if (ImGui::BeginCombo("##rwy", s.rwy.empty() ? "-" : s.rwy.c_str())) {
        for (const auto& r : s.runways) for (const auto& e : r.ends) {
            const bool pick = ImGui::Selectable(e.name.c_str(), e.name == s.rwy);
            probeItem("rwy:" + e.name);
            if (pick && e.name != s.rwy && a.setRunway) a.setRunway(e.name);
        }
        ImGui::EndCombo();
    }
    probeItem("runway");
    ImGui::SameLine(0, 24.f);
    if (toggle("lvp", tr("Low visibility (LVP)"), s.lvp) && a.setLvp) a.setLvp(!s.lvp);
    probeItem("lvp");
    ImGui::SameLine(0, 24.f);
    if (s.haveStopBars) {
        if (toggle("bars", tr("Stop bars"), s.stopBars) && a.setStopBars) a.setStopBars(!s.stopBars);
        probeItem("stopbars");
    } else {
        ImGui::TextUnformatted(tr("Stop bars")); ImGui::SameLine();
        if (ImGui::SmallButton(tr("on")) && a.setStopBars) a.setStopBars(true);
        probeItem("stopbars");
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("off")) && a.setStopBars) a.setStopBars(false);
    }
    ImGui::Separator();
    // ATIS and METAR
    ImGui::BeginChild("##atis", ImVec2(0, -(ImGui::GetFrameHeight() * 1.25f + ImGui::GetTextLineHeightWithSpacing() * 2.f + smallH() + 18.f)));
    ImGui::TextColored(p.accent, "ATIS");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, s.atis.empty() ? p.muted : p.text);
    ImGui::TextWrapped("%s", s.atis.empty() ? tr("no ATIS yet") : tw::displayText(s.atis).c_str());
    ImGui::PopStyleColor();
    if (!s.metar.empty()) { pushFont(g_fonts.small); ImGui::PushStyleColor(ImGuiCol_Text, p.muted); ImGui::TextWrapped("%s", s.metar.c_str()); ImGui::PopStyleColor(); popFont(g_fonts.small); }
    ImGui::EndChild();
    ImGui::Separator();
    // the light gun
    const tower::Aircraft* sel = findAc(s, s.selectedId);
    if (sel) { ImGui::TextUnformatted(tr("Light gun for")); ImGui::SameLine(); ImGui::TextColored(kindColour(*sel), "%s", sel->cs.c_str()); ImGui::SameLine(); muted(sel->onGround ? tr("(on the ground)") : tr("(in the air)")); }
    else muted(tr("Light gun: select an aircraft first (radar, strips or Delivery)"));
    const auto& sigs = tw::signals();
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (float)(sigs.size() - 1)) / (float)sigs.size();
    ImGui::BeginDisabled(!sel);
    for (size_t i = 0; i < sigs.size(); ++i) { if (i) ImGui::SameLine(); lightButton(sigs[i], s, sel, a, w); }
    ImGui::EndDisabled();
    pushFont(g_fonts.small);
    muted(tr("TowerTrainer decides: these are requests to its simulation."));
    popFont(g_fonts.small);
}

// ---------------------------------------------------------------- the connection banner
void drawBanner(const TowerFrame& f, const tower::State& s, const TowerActions& a) {
    const Palette& p = palette();
    const tw::Banner b = tw::bannerFor(s, f.code);
    if (!b.show) { light(p.ok, true); ImGui::SameLine(); ImGui::TextUnformatted(tr("Linked to TowerTrainer")); return; }
    const ImVec4 lc = b.level == 2 ? p.err : b.level == 1 ? p.warn : p.accent;
    const bool waiting = s.phase == tower::Phase::Connecting || (s.phase == tower::Phase::Linked && !s.haveAirport);
    if (waiting) spinner(7.f, s.now); else light(lc, true, 8.f);
    ImGui::SameLine();
    pushFont(g_fonts.heading);
    ImGui::PushStyleColor(ImGuiCol_Text, b.level ? lc : p.text);
    ImGui::TextWrapped("%s", b.title.c_str());
    ImGui::PopStyleColor();
    popFont(g_fonts.heading);
    if (!b.detail.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, p.muted); ImGui::TextWrapped("%s", b.detail.c_str()); ImGui::PopStyleColor(); }
    ImGui::Spacing();
    if (b.retry) {
        if (ImGui::Button(tr("Retry now")) && a.retry) a.retry();
        probeItem("retry");
        ImGui::SameLine();
    }
    if (ImGui::Button(tr("Tower settings")) && a.settings) a.settings();
    ImGui::SameLine();
    if (ImGui::Button(tr("Leave the tower")) && a.leave) a.leave();
}

// The pop-out control at the top right of every window (not in VR, where there is nowhere to pop out to).
void windowButton(const TowerFrame& f, const TowerActions& a) {
    if (f.inVR || !a.popOut) return;
    const Palette& p = palette();
    const float sz = ImGui::GetFrameHeight() * 0.85f;
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const ImVec2 at(wp.x + ws.x - sz - 6.f, wp.y + 5.f);
    ImGui::SetCursorScreenPos(at);
    const bool clicked = ImGui::InvisibleButton("##popout", ImVec2(sz, sz));
    const bool hov = ImGui::IsItemHovered();
    probeItem("popout");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(at, ImVec2(at.x + sz, at.y + sz), col(p.accent, 0.3f), 3.f);
    const ImU32 c = col(hov ? p.text : p.muted);
    const float m = sz * 0.22f;
    // a frame with an arrow out of its corner (in, when it is already out)
    dl->AddRect(ImVec2(at.x + m, at.y + m * 1.6f), ImVec2(at.x + sz - m * 1.6f, at.y + sz - m), c, 1.5f, 0, 1.3f);
    const ImVec2 t0(at.x + sz * 0.48f, at.y + sz * 0.52f), t1(at.x + sz - m * 0.7f, at.y + m * 0.7f);
    if (!f.poppedOut) { dl->AddLine(t0, t1, c, 1.6f); dl->AddLine(t1, ImVec2(t1.x - sz * 0.25f, t1.y), c, 1.6f); dl->AddLine(t1, ImVec2(t1.x, t1.y + sz * 0.25f), c, 1.6f); }
    else { dl->AddLine(t1, t0, c, 1.6f); dl->AddLine(t0, ImVec2(t0.x + sz * 0.25f, t0.y), c, 1.6f); dl->AddLine(t0, ImVec2(t0.x, t0.y - sz * 0.25f), c, 1.6f); }
    if (hov) ImGui::SetTooltip("%s", f.poppedOut ? tr("Back into X-Plane") : tr("Pop out into its own window"));
    if (clicked) a.popOut(f.which, !f.poppedOut);
    // the last thing in the window, so the cursor stays where the button left it
}
}  // namespace

// ---------------------------------------------------------------- public
TowerUi& towerUi() { return g_ui; }
void setTowerFonts(const Fonts& f) { g_fonts = f; }
void enableProbes(bool on) { g_probeOn = on; if (!on) g_probes.clear(); }
bool probesOn() { return g_probeOn; }
ProbeRect probe(const std::string& name) { auto it = g_probes.find(name); return it == g_probes.end() ? ProbeRect() : it->second; }
void clearProbes() { g_probes.clear(); }
void probeLast(const std::string& name) { probeItem(name); }

const char* towerWindowTitle(int which) {
    static const char* k[kTwCount] = {"Tower: ground radar", "Tower: strips", "Tower: Delivery", "Tower: frequency", "Tower: ATIS / operations", "Tower: TowerTrainer"};
    return which >= 0 && which < kTwCount ? k[which] : "Tower";
}

void drawTower(const TowerFrame& f, const tower::State& s, const TowerActions& a) {
    ImGui::PushID(f.which);
    switch (f.which) {
        case kTwRadar: drawRadar(f, s, a); break;
        case kTwStrips: drawStrips(f, s, a); break;
        case kTwDelivery: drawDelivery(f, s, a); break;
        case kTwFrequency: drawFrequency(f, s, a); break;
        case kTwOps: drawOps(f, s, a); break;
        default: drawBanner(f, s, a); break;
    }
    windowButton(f, a);
    ImGui::PopID();
}

}  // namespace ui2
}  // namespace xr
