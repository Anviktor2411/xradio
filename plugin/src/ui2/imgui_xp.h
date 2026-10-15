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

namespace xr {
namespace ui2 {

// Fonts for every window: Inter (OFL), body / heading / big / mono-ish, at the UI scale.
struct FontSet { ImFont* body = nullptr; ImFont* heading = nullptr; ImFont* big = nullptr; ImFont* mono = nullptr; };
ImFontAtlas* sharedAtlas();                 // built on first use
const FontSet& fonts();
void   rebuildFonts(float scale);           // after the UI scale changed (re-uploads the texture)
void   uploadFontTexture();                 // (re)creates the GL texture from the atlas; main thread, GL context

class XpWindow {
public:
    using DrawFn = std::function<void(XpWindow&)>;
    XpWindow(const std::string& title, int left, int top, int right, int bottom, bool decorated, DrawFn draw);
    ~XpWindow();
    XpWindow(const XpWindow&) = delete;
    XpWindow& operator=(const XpWindow&) = delete;

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
