// Put a short piece of text on the system clipboard.
//
// X-Plane's SDK has no clipboard call, and nothing drawn in an X-Plane window
// can be selected with the mouse, so a join code shown on screen is a join
// code you have to read out loud or photograph. This exists so a button can
// put it on the clipboard instead, ready to paste into whatever the pilot is
// already talking to their friends in.
//
// Windows has a clipboard in the operating system and we use it directly.
// macOS and Linux go through the small command-line tools that own the
// selection there; on Linux those are not always installed, and the answer
// then is to say so rather than to pretend it worked.
#pragma once

#include <string>

namespace xr {
namespace clipboard {

// True if the text is now on the clipboard. On failure `err` says why, in
// words a pilot can act on.
bool set(const std::string& text, std::string* err);

}  // namespace clipboard
}  // namespace xr
