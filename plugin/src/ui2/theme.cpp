#include "theme.h"

#include <cstring>
#include <string>

namespace xr {
namespace ui2 {

namespace {
Palette g_pal;
std::string g_theme = "dark";

Palette dark() {
    Palette p;
    p.bg = ImVec4(0.067f, 0.082f, 0.102f, 0.96f);
    p.panel = ImVec4(0.094f, 0.114f, 0.141f, 1.f);
    p.panelAlt = ImVec4(0.125f, 0.149f, 0.184f, 1.f);
    p.text = ImVec4(0.91f, 0.93f, 0.95f, 1.f);
    p.muted = ImVec4(0.56f, 0.61f, 0.67f, 1.f);
    p.line = ImVec4(0.20f, 0.24f, 0.29f, 1.f);
    p.accent = ImVec4(0.36f, 0.64f, 0.96f, 1.f);
    p.ok = ImVec4(0.33f, 0.85f, 0.45f, 1.f);
    p.warn = ImVec4(1.00f, 0.76f, 0.30f, 1.f);
    p.err = ImVec4(1.00f, 0.38f, 0.36f, 1.f);
    p.rx = p.ok;
    p.tx = p.err;
    p.atc = ImVec4(0.55f, 0.95f, 0.62f, 1.f);
    p.priv = ImVec4(1.00f, 0.85f, 0.45f, 1.f);
    p.ai = ImVec4(0.62f, 0.72f, 0.85f, 1.f);
    return p;
}
Palette light() {
    Palette p;
    p.bg = ImVec4(0.95f, 0.96f, 0.97f, 0.98f);
    p.panel = ImVec4(1.f, 1.f, 1.f, 1.f);
    p.panelAlt = ImVec4(0.92f, 0.94f, 0.96f, 1.f);
    p.text = ImVec4(0.10f, 0.12f, 0.15f, 1.f);
    p.muted = ImVec4(0.42f, 0.46f, 0.52f, 1.f);
    p.line = ImVec4(0.78f, 0.81f, 0.85f, 1.f);
    p.accent = ImVec4(0.16f, 0.45f, 0.85f, 1.f);
    p.ok = ImVec4(0.12f, 0.60f, 0.25f, 1.f);
    p.warn = ImVec4(0.80f, 0.50f, 0.05f, 1.f);
    p.err = ImVec4(0.82f, 0.16f, 0.14f, 1.f);
    p.rx = p.ok;
    p.tx = p.err;
    p.atc = ImVec4(0.08f, 0.50f, 0.22f, 1.f);
    p.priv = ImVec4(0.70f, 0.45f, 0.00f, 1.f);
    p.ai = ImVec4(0.30f, 0.40f, 0.55f, 1.f);
    return p;
}
}  // namespace

const Palette& palette() { return g_pal; }
ImU32 col(const ImVec4& v, float alpha) { return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * alpha)); }

void applyTheme(const char* name, float scale) {
    g_theme = (name && strcmp(name, "light") == 0) ? "light" : "dark";
    g_pal = g_theme == "light" ? light() : dark();
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 6.f; s.FrameRounding = 4.f; s.GrabRounding = 4.f; s.TabRounding = 4.f; s.ChildRounding = 4.f; s.PopupRounding = 4.f;
    s.WindowPadding = ImVec2(10, 10); s.FramePadding = ImVec2(8, 5); s.ItemSpacing = ImVec2(8, 6); s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize = 12.f; s.GrabMinSize = 10.f; s.WindowBorderSize = 0.f; s.FrameBorderSize = 0.f; s.TabBorderSize = 0.f;
    const Palette& p = g_pal;
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = p.text; c[ImGuiCol_TextDisabled] = p.muted;
    c[ImGuiCol_WindowBg] = p.bg; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = p.panel;
    c[ImGuiCol_Border] = p.line; c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.panelAlt; c[ImGuiCol_FrameBgHovered] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f); c[ImGuiCol_FrameBgActive] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.55f);
    c[ImGuiCol_TitleBg] = p.panel; c[ImGuiCol_TitleBgActive] = p.panel; c[ImGuiCol_TitleBgCollapsed] = p.panel; c[ImGuiCol_MenuBarBg] = p.panel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = p.line; c[ImGuiCol_ScrollbarGrabHovered] = p.muted; c[ImGuiCol_ScrollbarGrabActive] = p.accent;
    c[ImGuiCol_CheckMark] = p.accent; c[ImGuiCol_SliderGrab] = p.accent; c[ImGuiCol_SliderGrabActive] = p.accent;
    c[ImGuiCol_Button] = p.panelAlt; c[ImGuiCol_ButtonHovered] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.6f); c[ImGuiCol_ButtonActive] = p.accent;
    c[ImGuiCol_Header] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.25f); c[ImGuiCol_HeaderHovered] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.45f); c[ImGuiCol_HeaderActive] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.6f);
    c[ImGuiCol_Separator] = p.line; c[ImGuiCol_SeparatorHovered] = p.accent; c[ImGuiCol_SeparatorActive] = p.accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ResizeGripHovered] = p.accent; c[ImGuiCol_ResizeGripActive] = p.accent;
    c[ImGuiCol_Tab] = p.panel; c[ImGuiCol_TabHovered] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.5f); c[ImGuiCol_TabSelected] = p.panelAlt;
    c[ImGuiCol_TabDimmed] = p.panel; c[ImGuiCol_TabDimmedSelected] = p.panelAlt;
    c[ImGuiCol_TableHeaderBg] = p.panelAlt; c[ImGuiCol_TableBorderStrong] = p.line; c[ImGuiCol_TableBorderLight] = p.line;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableRowBgAlt] = ImVec4(p.panelAlt.x, p.panelAlt.y, p.panelAlt.z, 0.4f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.35f);
    c[ImGuiCol_NavCursor] = p.accent;
    if (scale > 0.f && scale != 1.f) s.ScaleAllSizes(scale);
}

void light(const ImVec4& colour, bool on, float r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 c(p.x + r + 2, p.y + h * 0.5f);
    dl->AddCircleFilled(c, r, col(colour, on ? 1.f : 0.18f));
    if (on) dl->AddCircle(c, r + 2.5f, col(colour, 0.35f), 0, 1.5f);
    ImGui::Dummy(ImVec2(r * 2 + 6, h));
}

void signalBarsWidget(int bars, int max, const ImVec4& colour) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight(), w = 4.f, gap = 2.f;
    for (int i = 0; i < max; ++i) {
        const float bh = h * (0.35f + 0.65f * (float)(i + 1) / (float)max);
        const ImVec2 a(p.x + i * (w + gap), p.y + h - bh), b(a.x + w, p.y + h);
        dl->AddRectFilled(a, b, col(colour, i < bars ? 1.f : 0.2f), 1.f);
    }
    ImGui::Dummy(ImVec2(max * (w + gap), h));
}

void meter(float level01, const ImVec4& colour, float w, float h) {
    if (level01 < 0.f) level01 = 0.f;
    if (level01 > 1.f) level01 = 1.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float y = p.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), col(palette().panelAlt), 3.f);
    dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * level01, y + h), col(level01 > 0.9f ? palette().err : colour), 3.f);
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
}

void badge(const char* text, const ImVec4& colour) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const float pad = 4.f;
    dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + sz.x + pad * 2, p.y + sz.y + 1), col(colour, 0.22f), 3.f);
    dl->AddText(ImVec2(p.x + pad, p.y + 1), col(colour), text);
    ImGui::Dummy(ImVec2(sz.x + pad * 2, sz.y + 2));
}

}  // namespace ui2
}  // namespace xr
