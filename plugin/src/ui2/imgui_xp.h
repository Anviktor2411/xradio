// Dear ImGui inside an X-Plane window: one ImGui context per XPLM window, the font atlas shared, drawn with
// plain OpenGL through XPLMSetGraphicsState / XPLMBindTexture2d (the texture is X-Plane's, so it stays valid
// through Vulkan/Metal's OpenGL bridge), input from the window callbacks, pop-out and VR through the SDK's
// positioning modes. The approach is the one ImgWindow (xsquawkbox, LiveTraffic -- BSD) made standard; this
// is a fresh, small implementation under XRadio's MIT licence.
#pragma once

#include <functional>
#include <string>

#include "XPLMDisplay.h"
#include "imgui.h"

#ifdef small
#  undef small          // <rpcndr.h> (via <windows.h>): #define small char
#endif

namespace xr {
namespace ui2 {

// Makes `ctx` the current ImGui context for as long as this object lives and puts the previous one back
// after, however the scope is left -- so a window drawing, or an input callback, never leaves its context
// current for whoever runs next. Nests. v0.6.0 crashed X-Plane on Apply because one switch was not undone:
// the rest of the main window's frame then ran against the overlay's context, which was not in a frame.
class ContextGuard {
public:
    explicit ContextGuard(ImGuiContext* ctx) : prev_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(ctx); }
    ~ContextGuard() { ImGui::SetCurrentContext(prev_); }
    ContextGuard(const ContextGuard&) = delete;
    ContextGuard& operator=(const ContextGuard&) = delete;
private:
    ImGuiContext* prev_;
};

// Fonts for every window: Inter (OFL), body / heading / big / mono-ish / small (radar labels), at the UI scale.
struct FontSet { ImFont* body = nullptr; ImFont* heading = nullptr; ImFont* big = nullptr; ImFont* mono = nullptr; ImFont* small = nullptr; };
ImFontAtlas* sharedAtlas();                 // built on first use
const FontSet& fonts();
// The atlas is shared by every window's context, so it is only rebuilt between frames: requestFontRebuild()
// marks it, and the next window to start a frame rebuilds it (and re-uploads the texture) before NewFrame.
// rebuildFonts() is the same, done at once when no window is in the middle of a frame (startup).
void   requestFontRebuild(float scale);
void   rebuildFonts(float scale);
bool   fontRebuildPending();
int    framesInFlight();                    // windows between their NewFrame and Render right now
void   uploadFontTexture();                 // (re)creates the GL texture from the atlas; main thread, GL context

class XpWindow {
public:
    using DrawFn = std::function<void(XpWindow&)>;
    XpWindow(const std::string& title, int left, int top, int right, int bottom, bool decorated, DrawFn draw);
    ~XpWindow();
    XpWindow(const XpWindow&) = delete;
    XpWindow& operator=(const XpWindow&) = delete;

    // Runs in this window's own context just before its NewFrame: where the theme goes (host.cpp), since a
    // style changed half-way through a frame is applied to half a window.
    void setBeforeFrame(DrawFn fn) { before_ = std::move(fn); }

    void setVisible(bool on);
    bool visible() const;
    void bringToFront();
    void geometry(int* l, int* t, int* r, int* b) const;   // in the space the window is drawn in (sim / OS / VR)
    void setGeometry(int l, int t, int r, int b);
    void popOut(bool on);
    bool poppedOut() const;
    void toVR(bool on);
    bool inVR() const;
    float width() const { return w_; }
    float height() const { return h_; }
    XPLMWindowID id() const { return win_; }
    ImGuiContext* context() const { return ctx_; }
    void setResizingLimits(int minW, int minH, int maxW, int maxH);
    void setPassThrough(bool on) { passThrough_ = on; }    // overlay: clicks outside ImGui's interest fall through
    void requestFocus(bool on);

private:
    static void drawCb(XPLMWindowID, void* ref);
    static int  clickCb(XPLMWindowID, int x, int y, XPLMMouseStatus, void* ref);
    static int  rightClickCb(XPLMWindowID, int x, int y, XPLMMouseStatus, void* ref);
    static int  wheelCb(XPLMWindowID, int x, int y, int wheel, int clicks, void* ref);
    static void keyCb(XPLMWindowID, char key, XPLMKeyFlags, char vk, void* ref, int losingFocus);
    static XPLMCursorStatus cursorCb(XPLMWindowID, int x, int y, void* ref);

    void frame();
    void render(ImDrawData* dd, int l, int t, int r, int b);
    void mouse(int x, int y, int l, int t);
    int  mouseEvent(int x, int y, XPLMMouseStatus st, int button);

    XPLMWindowID  win_ = nullptr;
    ImGuiContext* ctx_ = nullptr;
    DrawFn        draw_;
    DrawFn        before_;
    float         w_ = 0.f, h_ = 0.f;
    double        lastTime_ = 0.0;
    bool          passThrough_ = false;
    bool          hasFocus_ = false;
    bool          decorated_ = true;
    int           dragButton_ = -1;
};

float uiScale();
void  setUiScale(float s);

}  // namespace ui2
}  // namespace xr
