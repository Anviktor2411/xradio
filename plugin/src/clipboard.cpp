#include "clipboard.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cstdlib>
#endif

namespace xr {
namespace clipboard {

namespace {

// What may go on the clipboard from here: a join code, an address, a
// frequency. Everything printable and ordinary, nothing that could end up
// being read as shell syntax on the two platforms where this goes through a
// command. Refusing is safer than quoting, and nothing XRadio copies needs
// anything outside this.
bool safeText(const std::string& t) {
    if (t.empty() || t.size() > 200) return false;
    for (char c : t) {
        // c != 0 first, deliberately: strchr answers "yes, at the end" for a
        // null byte, so without it an embedded NUL would pass as acceptable.
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        (c != '\0' && strchr(" .:-_/@+", c) != nullptr);
        if (!ok) return false;
    }
    return true;
}

#ifndef _WIN32
// Hand the text to a clipboard tool on its standard input. Returns false if
// the tool is not there or would not take it.
bool pipeTo(const char* cmd, const std::string& text) {
    FILE* p = popen(cmd, "w");
    if (!p) return false;
    const size_t n = fwrite(text.data(), 1, text.size(), p);
    const int rc = pclose(p);
    return n == text.size() && rc == 0;
}

bool haveTool(const char* name) {
    std::string probe = "command -v ";
    probe += name;
    probe += " > /dev/null 2>&1";
    return system(probe.c_str()) == 0;
}
#endif

}  // namespace

#ifdef _WIN32

bool set(const std::string& text, std::string* err) {
    if (!safeText(text)) { *err = "there is nothing to copy"; return false; }
    if (!OpenClipboard(nullptr)) { *err = "another program is holding the clipboard"; return false; }

    bool ok = false;
    if (EmptyClipboard()) {
        // The clipboard takes ownership of this, so it is deliberately not
        // freed on the success path.
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (mem) {
            if (void* dst = GlobalLock(mem)) {
                memcpy(dst, text.c_str(), text.size() + 1);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_TEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
    }
    CloseClipboard();
    if (!ok && err->empty()) *err = "Windows would not take it";
    return ok;
}

#elif defined(__APPLE__)

bool set(const std::string& text, std::string* err) {
    if (!safeText(text)) { *err = "there is nothing to copy"; return false; }
    if (pipeTo("pbcopy", text)) return true;
    *err = "pbcopy would not take it";
    return false;
}

#else

bool set(const std::string& text, std::string* err) {
    if (!safeText(text)) { *err = "there is nothing to copy"; return false; }
    // Wayland first, then the two X tools. Which of these exists is a
    // property of the machine, not of the session, so trying all three is
    // the whole of the portability story on Linux.
    static const char* const kTools[][2] = {
        {"wl-copy", "wl-copy"},
        {"xclip",   "xclip -selection clipboard"},
        {"xsel",    "xsel --clipboard --input"},
    };
    bool sawOne = false;
    for (const auto& t : kTools) {
        if (!haveTool(t[0])) continue;
        sawOne = true;
        if (pipeTo(t[1], text)) return true;
    }
    *err = sawOne ? "the clipboard tool would not take it"
                  : "no clipboard tool here -- install xclip, xsel or wl-clipboard";
    return false;
}

#endif

}  // namespace clipboard
}  // namespace xr
