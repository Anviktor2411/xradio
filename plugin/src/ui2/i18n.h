// The window's own words in English and Estonian (radio phraseology always stays English).
#pragma once
#include <string>

namespace xr {
namespace ui2 {

void        setLanguage(const std::string& lang);   // "en" or "et"
const char* language();
const char* tr(const char* key);                    // the key itself when there is no translation

}  // namespace ui2
}  // namespace xr
