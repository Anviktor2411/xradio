#include "imgui_xp.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

// XRADIO_UI_HEADLESS (the test builds): everything but the OpenGL calls, so the windows can be drawn and clicked
// through tools/harness/xplm_stub.cpp on a machine with no GL context.
#ifndef XRADIO_UI_HEADLESS
#  ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#      define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <GL/gl.h>
#  elif defined(__APPLE__)
#    define GL_SILENCE_DEPRECATION 1
#    include <OpenGL/gl.h>
#  else
#    include <GL/gl.h>
#  endif
#endif

#include "XPLMDataAccess.h"
#include "XPLMGraphics.h"
#include "XPLMUtilities.h"

#include "imgui_internal.h"    // ErrorRecoveryStoreState / ErrorRecoveryTryToRecoverState
#include "font_inter.h"

namespace xr {
namespace ui2 {

void noteKey(int vk, bool down);           // app_ui.cpp (here, not block scope: MSVC would look for ::noteKey)

namespace {
ImFontAtlas* g_atlas = nullptr;
FontSet      g_fonts;
int          g_fontTex = 0;
float        g_scale = 1.f;
bool         g_texUploaded = false;
bool         g_fontsDirty = false;         // requestFontRebuild(): rebuild before the next frame starts
float        g_fontsWantScale = 1.f;
int          g_framesInFlight = 0;          // windows between NewFrame and Render (the draw callbacks run one at a time)
XPLMDataRef  g_fbW = nullptr;
int          g_drawErrors = 0;              // exceptions out of a window's drawing, logged the first few times

void logLine(const char* what, const char* detail) {
    char line[600];
    snprintf(line, sizeof line, "XRadio: %s%s%s\n", what, detail && *detail ? ": " : "", detail ? detail : "");
    XPLMDebugString(line);
}

void buildFonts(float scale);

const ImWchar* glyphRanges() {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   // Latin + Latin-1 (õ ä ö ü)
        0x0100, 0x017F,   // Latin Extended-A (š ž ...)
        0x02C6, 0x02DC,
        0x2010, 0x2027,   // dashes, quotes, bullets
        0x2190, 0x21FF,   // arrows
        0x2500, 0x25FF,   // box drawing, geometric shapes
        0x2600, 0x26FF,   // misc symbols (what the font has)
        0,
    };
    return ranges;
}

double nowS() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

#ifndef XRADIO_UI_HEADLESS
// Framebuffer pixels per boxel (HiDPI): the scissor rectangle is in pixels, everything else in boxels.
float pixelsPerBoxel() {
    int sw = 0, sh = 0;
    XPLMGetScreenSize(&sw, &sh);
    if (!g_fbW) g_fbW = XPLMFindDataRef("sim/graphics/view/window_width");
    const int fb = g_fbW ? XPLMGetDatai(g_fbW) : 0;
    if (sw > 0 && fb > 0) { const float r = (float)fb / (float)sw; if (r > 0.5f && r < 4.f) return r; }
    return 1.f;
}
#endif

ImGuiKey mapKey(unsigned char vk) {
    switch (vk) {
        case 0x08: return ImGuiKey_Backspace; case 0x09: return ImGuiKey_Tab; case 0x0D: return ImGuiKey_Enter;
        case 0x1B: return ImGuiKey_Escape; case 0x20: return ImGuiKey_Space; case 0x21: return ImGuiKey_PageUp;
        case 0x22: return ImGuiKey_PageDown; case 0x23: return ImGuiKey_End; case 0x24: return ImGuiKey_Home;
        case 0x25: return ImGuiKey_LeftArrow; case 0x26: return ImGuiKey_UpArrow; case 0x27: return ImGuiKey_RightArrow;
        case 0x28: return ImGuiKey_DownArrow; case 0x2D: return ImGuiKey_Insert; case 0x2E: return ImGuiKey_Delete;
        default: break;
    }
    if (vk >= 0x41 && vk <= 0x5A) return (ImGuiKey)(ImGuiKey_A + (vk - 0x41));
    if (vk >= 0x30 && vk <= 0x39) return (ImGuiKey)(ImGuiKey_0 + (vk - 0x30));
    return ImGuiKey_None;
}
}  // namespace

float uiScale() { return g_scale; }
void  setUiScale(float s) { g_scale = s < 0.5f ? 0.5f : (s > 3.f ? 3.f : s); }

ImFontAtlas* sharedAtlas() {
    if (!g_atlas) { g_atlas = IM_NEW(ImFontAtlas)(); buildFonts(g_scale); }
    return g_atlas;
}

const FontSet& fonts() { return g_fonts; }
int framesInFlight() { return g_framesInFlight; }
bool fontRebuildPending() { return g_fontsDirty; }

void requestFontRebuild(float scale) { g_fontsWantScale = scale; g_fontsDirty = true; }

void rebuildFonts(float scale) {
    if (g_framesInFlight > 0) { requestFontRebuild(scale); return; }   // a frame still draws with the old fonts
    buildFonts(scale);
}

namespace {
// Clears the shared atlas and builds it again: every ImFont* handed out so far is gone afterwards. Only ever
// called with no window in the middle of a frame (rebuildFonts / XpWindow::frame check).
void buildFonts(float scale) {
    if (!g_atlas) g_atlas = IM_NEW(ImFontAtlas)();
    g_fontsDirty = false;
    g_scale = scale;
    g_atlas->Clear();
    ImFontConfig cfg;
    cfg.OversampleH = 2; cfg.OversampleV = 1; cfg.PixelSnapH = false;
    const float body = 15.f * scale;
    g_fonts.body = g_atlas->AddFontFromMemoryCompressedTTF(InterRegular_compressed_data, (int)InterRegular_compressed_size, body, &cfg, glyphRanges());
    g_fonts.heading = g_atlas->AddFontFromMemoryCompressedTTF(InterSemiBold_compressed_data, (int)InterSemiBold_compressed_size, 17.f * scale, &cfg, glyphRanges());
    g_fonts.big = g_atlas->AddFontFromMemoryCompressedTTF(InterSemiBold_compressed_data, (int)InterSemiBold_compressed_size, 26.f * scale, &cfg, glyphRanges());
    g_fonts.small = g_atlas->AddFontFromMemoryCompressedTTF(InterRegular_compressed_data, (int)InterRegular_compressed_size, 12.5f * scale, &cfg, glyphRanges());
    g_fonts.mono = g_fonts.body;
    g_atlas->Build();
    g_texUploaded = false;
}
}  // namespace

void uploadFontTexture() {
    if (!g_atlas) sharedAtlas();
    unsigned char* pixels = nullptr; int w = 0, h = 0;
    g_atlas->GetTexDataAsRGBA32(&pixels, &w, &h);
    if (!g_fontTex) XPLMGenerateTextureNumbers(&g_fontTex, 1);
    XPLMBindTexture2d(g_fontTex, 0);
#ifndef XRADIO_UI_HEADLESS
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
#else
    // what glTexImage2D would read: every pixel, so a sanitizer sees an atlas freed under it
    unsigned sum = 0;
    for (int i = 0; pixels && i < w * h * 4; i += 4) sum += pixels[i + 3];
    (void)sum;
#endif
    g_atlas->SetTexID((ImTextureID)(intptr_t)(g_fontTex ? g_fontTex : 1));
    g_atlas->ClearTexData();
    g_texUploaded = true;
}

// ---------------------------------------------------------------- the window
XpWindow::XpWindow(const std::string& title, int left, int top, int right, int bottom, bool decorated, DrawFn draw)
    : draw_(std::move(draw)), decorated_(decorated) {
    // CreateContext leaves the new context current when there was none; the guard puts back what was
    ContextGuard keep(ImGui::GetCurrentContext());
    ctx_ = ImGui::CreateContext(sharedAtlas());
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.ConfigErrorRecoveryEnableTooltip = false;   // a pilot is not the audience for ImGui's own error tooltip
    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    p.left = left; p.top = top; p.right = right; p.bottom = bottom;
    p.visible = 0;
    p.drawWindowFunc = drawCb;
    p.handleMouseClickFunc = clickCb;
    p.handleRightClickFunc = rightClickCb;
    p.handleMouseWheelFunc = wheelCb;
    p.handleKeyFunc = keyCb;
    p.handleCursorFunc = cursorCb;
    p.refcon = this;
    p.decorateAsFloatingWindow = decorated ? xplm_WindowDecorationRoundRectangle : xplm_WindowDecorationSelfDecoratedResizable;
    p.layer = xplm_WindowLayerFloatingWindows;
    win_ = XPLMCreateWindowEx(&p);
    if (win_) {
        XPLMSetWindowTitle(win_, title.c_str());
        XPLMSetWindowPositioningMode(win_, xplm_WindowPositionFree, -1);
    }
    lastTime_ = nowS();
}

XpWindow::~XpWindow() {
    if (win_) XPLMDestroyWindow(win_);
    if (ctx_) ImGui::DestroyContext(ctx_);
}

void XpWindow::setVisible(bool on) { if (win_) XPLMSetWindowIsVisible(win_, on ? 1 : 0); }
bool XpWindow::visible() const { return win_ && XPLMGetWindowIsVisible(win_); }
void XpWindow::bringToFront() { if (win_) XPLMBringWindowToFront(win_); }
void XpWindow::setResizingLimits(int minW, int minH, int maxW, int maxH) { if (win_) XPLMSetWindowResizingLimits(win_, minW, minH, maxW, maxH); }
bool XpWindow::poppedOut() const { return win_ && XPLMWindowIsPoppedOut(win_); }
bool XpWindow::inVR() const { return win_ && XPLMWindowIsInVR(win_); }
void XpWindow::popOut(bool on) { if (win_) XPLMSetWindowPositioningMode(win_, on ? xplm_WindowPopOut : xplm_WindowPositionFree, -1); }
void XpWindow::toVR(bool on) { if (win_) XPLMSetWindowPositioningMode(win_, on ? xplm_WindowVR : xplm_WindowPositionFree, -1); }
void XpWindow::setGeometry(int l, int t, int r, int b) { if (win_) XPLMSetWindowGeometry(win_, l, t, r, b); }
void XpWindow::requestFocus(bool on) {
    if (!win_) return;
    if (on && !hasFocus_) { XPLMTakeKeyboardFocus(win_); hasFocus_ = true; }
    else if (!on && hasFocus_) { XPLMTakeKeyboardFocus(nullptr); hasFocus_ = false; }
}

void XpWindow::geometry(int* l, int* t, int* r, int* b) const {
    int L = 0, T = 0, R = 0, B = 0;
    if (!win_) { *l = *t = *r = *b = 0; return; }
    if (XPLMWindowIsInVR(win_)) {
        int w = 0, h = 0;
        XPLMGetWindowGeometryVR(win_, &w, &h);
        L = 0; T = h; R = w; B = 0;
    } else if (XPLMWindowIsPoppedOut(win_)) {
        XPLMGetWindowGeometryOS(win_, &L, &T, &R, &B);
    } else {
        XPLMGetWindowGeometry(win_, &L, &T, &R, &B);
    }
    *l = L; *t = T; *r = R; *b = B;
}

void XpWindow::drawCb(XPLMWindowID, void* ref) { ((XpWindow*)ref)->frame(); }

void XpWindow::frame() {
    if (!win_ || !ctx_) return;
    ContextGuard guard(ctx_);
    // The shared atlas changes only here, before this window's frame begins and with no other window in the
    // middle of one: a frame in flight draws with the fonts it started with, and clearing them under it frees
    // what its draw lists still point at.
    if (g_fontsDirty && g_framesInFlight == 0) buildFonts(g_fontsWantScale);
    if (!g_texUploaded) uploadFontTexture();
    int l, t, r, b;
    geometry(&l, &t, &r, &b);
    w_ = (float)(r - l); h_ = (float)(t - b);
    if (w_ < 10.f || h_ < 10.f) return;
    if (before_) before_(*this);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w_, h_);
    const double now = nowS();
    io.DeltaTime = (float)(now - lastTime_ > 0.0005 ? now - lastTime_ : 0.0005);
    lastTime_ = now;
    ImGui::NewFrame();
    ++g_framesInFlight;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w_, h_));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!decorated_) flags |= ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("##xr", nullptr, flags)) {
        // An exception must not leave through X-Plane's draw callback (that ends the sim), nor leave this
        // context with windows and stacks half pushed: ImGui unwinds to where the drawing started.
        ImGuiErrorRecoveryState before;
        ImGui::ErrorRecoveryStoreState(&before);
        try {
            if (draw_) draw_(*this);
        } catch (const std::exception& e) {
            ImGui::ErrorRecoveryTryToRecoverState(&before);
            if (++g_drawErrors <= 5) logLine("window drawing failed", e.what());
        } catch (...) {
            ImGui::ErrorRecoveryTryToRecoverState(&before);
            if (++g_drawErrors <= 5) logLine("window drawing failed", "unknown exception");
        }
    }
    ImGui::End();
    ImGui::Render();
    --g_framesInFlight;
    render(ImGui::GetDrawData(), l, t, r, b);
    // keyboard: take X-Plane's keys only while a text field wants them
    requestFocus(io.WantTextInput);
}

#ifdef XRADIO_UI_HEADLESS
// No OpenGL in the test builds: read everything the renderer would read -- each list's vertices and indices,
// each command's texture -- so a sanitizer catches draw data pointing at freed memory.
void XpWindow::render(ImDrawData* dd, int l, int t, int r, int b) {
    (void)l; (void)t; (void)r; (void)b;
    if (!dd) return;
    float sum = 0.f;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (int v = 0; v < cl->VtxBuffer.Size; ++v) sum += cl->VtxBuffer[v].pos.x + cl->VtxBuffer[v].uv.y;
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd& cmd = cl->CmdBuffer[c];
            if (cmd.UserCallback) continue;
            for (unsigned e = 0; e < cmd.ElemCount; ++e) sum += (float)cl->IdxBuffer[(int)(cmd.IdxOffset + e)];
            sum += (float)(intptr_t)cmd.GetTexID();
        }
    }
    (void)sum;
}
#else
void XpWindow::render(ImDrawData* dd, int l, int t, int r, int b) {
    if (!dd || dd->CmdListsCount == 0) return;
    (void)r;
    const float ppb = poppedOut() || inVR() ? 1.f : pixelsPerBoxel();
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_SCISSOR_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    // ImGui's origin is the window's top-left with y down; X-Plane's is bottom-left with y up
    glTranslatef((float)l, (float)t, 0.f);
    glScalef(1.f, -1.f, 1.f);
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const ImDrawVert* vtx = cl->VtxBuffer.Data;
        const ImDrawIdx* idx = cl->IdxBuffer.Data;
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtx + offsetof(ImDrawVert, pos)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtx + offsetof(ImDrawVert, uv)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert), (const GLvoid*)((const char*)vtx + offsetof(ImDrawVert, col)));
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd& cmd = cl->CmdBuffer[c];
            if (cmd.UserCallback) { cmd.UserCallback(cl, &cmd); continue; }
            const ImVec4& clip = cmd.ClipRect;
            // scissor in framebuffer pixels: window left + clip x, window top - clip bottom
            const int sx = (int)((l + clip.x) * ppb), sy = (int)((t - clip.w) * ppb);
            const int sw = (int)((clip.z - clip.x) * ppb), sh = (int)((clip.w - clip.y) * ppb);
            if (sw <= 0 || sh <= 0) continue;
            glScissor(sx, poppedOut() ? (int)((h_ - clip.w) * ppb) + (b < 0 ? 0 : 0) : sy, sw, sh);
            XPLMBindTexture2d((int)(intptr_t)cmd.GetTexID(), 0);
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount, sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, idx + cmd.IdxOffset);
        }
    }
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
}
#endif

void XpWindow::mouse(int x, int y, int l, int t) {
    ContextGuard guard(ctx_);
    ImGui::GetIO().AddMousePosEvent((float)(x - l), (float)(t - y));
}

int XpWindow::mouseEvent(int x, int y, XPLMMouseStatus st, int button) {
    if (!ctx_) return 0;
    int l, t, r, b; geometry(&l, &t, &r, &b);
    mouse(x, y, l, t);
    ContextGuard guard(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    if (st == xplm_MouseDown) {
        if (passThrough_ && !io.WantCaptureMouse) return 0;
        io.AddMouseButtonEvent(button, true);
        dragButton_ = button;
    } else if (st == xplm_MouseUp) {
        io.AddMouseButtonEvent(button, false);
        dragButton_ = -1;
    }
    return 1;
}

int XpWindow::clickCb(XPLMWindowID, int x, int y, XPLMMouseStatus st, void* ref) { return ((XpWindow*)ref)->mouseEvent(x, y, st, 0); }
int XpWindow::rightClickCb(XPLMWindowID, int x, int y, XPLMMouseStatus st, void* ref) { return ((XpWindow*)ref)->mouseEvent(x, y, st, 1); }

int XpWindow::wheelCb(XPLMWindowID, int x, int y, int wheel, int clicks, void* ref) {
    XpWindow* w = (XpWindow*)ref;
    if (!w->ctx_) return 0;
    int l, t, r, b; w->geometry(&l, &t, &r, &b);
    w->mouse(x, y, l, t);
    ContextGuard guard(w->ctx_);
    ImGuiIO& io = ImGui::GetIO();
    if (w->passThrough_ && !io.WantCaptureMouse) return 0;
    if (wheel == 0) io.AddMouseWheelEvent(0.f, (float)clicks); else io.AddMouseWheelEvent((float)clicks, 0.f);
    return 1;
}

XPLMCursorStatus XpWindow::cursorCb(XPLMWindowID, int x, int y, void* ref) {
    XpWindow* w = (XpWindow*)ref;
    if (!w->ctx_) return xplm_CursorDefault;
    int l, t, r, b; w->geometry(&l, &t, &r, &b);
    w->mouse(x, y, l, t);
    return xplm_CursorDefault;
}

void XpWindow::keyCb(XPLMWindowID, char key, XPLMKeyFlags flags, char vk, void* ref, int losingFocus) {
    XpWindow* w = (XpWindow*)ref;
    if (!w->ctx_) return;
    ContextGuard guard(w->ctx_);
    ImGuiIO& io = ImGui::GetIO();
    if (losingFocus) { w->hasFocus_ = false; io.AddFocusEvent(false); io.AddFocusEvent(true); return; }
    const bool down = (flags & xplm_DownFlag) != 0;
    const bool up = (flags & xplm_UpFlag) != 0;
    io.AddKeyEvent(ImGuiMod_Shift, (flags & xplm_ShiftFlag) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & xplm_ControlFlag) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (flags & xplm_OptionAltFlag) != 0);
    const ImGuiKey k = mapKey((unsigned char)vk);
    if (k != ImGuiKey_None) io.AddKeyEvent(k, down || (!up && !down));
    if (up && k != ImGuiKey_None) io.AddKeyEvent(k, false);
    const unsigned char ch = (unsigned char)key;
    if ((down || (!up && !down)) && ch >= 0x20 && ch != 0x7F && !(flags & xplm_ControlFlag)) io.AddInputCharacter(ch);
    noteKey((unsigned char)vk, down || (!up && !down));
}

}  // namespace ui2
}  // namespace xr
