#include "update.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winhttp.h>
#endif

namespace xr {
namespace update {

namespace {

// The releases endpoint of the project's own repository. A test points this
// somewhere local instead.
const char* kDefaultUrl =
    "https://api.github.com/repos/Anviktor2411/xradio/releases/latest";

std::mutex        g_mx;
Info              g_info;
std::thread       g_worker;
std::atomic<bool> g_started{false};

std::string url() {
    if (const char* forced = getenv("XRADIO_UPDATE_URL")) return forced;
    return kDefaultUrl;
}

// Only the characters a URL needs. Everything else is refused rather than
// escaped, because this string ends up on a command line on two of the three
// platforms and "clever" quoting is how that goes wrong.
bool safeUrl(const std::string& u) {
    if (u.size() < 8 || u.size() > 300) return false;
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) return false;
    for (char c : u) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        strchr(":/._~?=&%-+", c) != nullptr;
        if (!ok) return false;
    }
    return true;
}

#ifdef _WIN32
// WinHTTP: in the operating system already, and it does not flash a console
// window in front of a pilot who is flying.
bool fetch(const std::string& u, std::string* out, std::string* err) {
    std::wstring wide(u.begin(), u.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {0}, path[1024] = {0};
    parts.lpszHostName = host;     parts.dwHostNameLength = 255;
    parts.lpszUrlPath  = path;     parts.dwUrlPathLength  = 1023;
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) { *err = "bad address"; return false; }

    HINTERNET session = WinHttpOpen(L"XRadio", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *err = "no internet access"; return false; }
    WinHttpSetTimeouts(session, 4000, 4000, 6000, 6000);

    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0);
    if (conn) {
        const DWORD flags = (parts.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD got = 0;
                char buf[4096];
                while (WinHttpReadData(req, buf, sizeof(buf), &got) && got > 0) {
                    out->append(buf, got);
                    if (out->size() > 64 * 1024) break;   // a release blob is ~2 KB
                }
                ok = !out->empty();
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    if (!ok && err->empty()) *err = "could not reach the update site";
    return ok;
}
#else
// curl is part of macOS and of every Linux X-Plane runs on, and it brings
// the TLS this needs -- which the plugin itself has no business carrying.
bool fetch(const std::string& u, std::string* out, std::string* err) {
    std::string cmd = "curl -fsS --max-time 6 -A XRadio ";
    cmd += "'" + u + "' 2>/dev/null";          // safeUrl() has excluded quotes
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { *err = "could not run curl"; return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) {
        out->append(buf, n);
        if (out->size() > 64 * 1024) break;
    }
    const int rc = pclose(p);
    if (rc != 0 || out->empty()) {
        *err = "could not reach the update site";
        return false;
    }
    return true;
}
#endif

// The two fields that matter, without dragging in a JSON library for a
// document we did not have to parse in the first place.
std::string jsonString(const std::string& body, const std::string& key,
                       const std::string& mustContain = "") {
    size_t at = 0;
    const std::string needle = "\"" + key + "\"";
    while ((at = body.find(needle, at)) != std::string::npos) {
        size_t colon = body.find(':', at + needle.size());
        if (colon == std::string::npos) return "";
        size_t open = body.find('"', colon);
        if (open == std::string::npos) return "";
        size_t close = body.find('"', open + 1);
        if (close == std::string::npos) return "";
        const std::string value = body.substr(open + 1, close - open - 1);
        if (mustContain.empty() || value.find(mustContain) != std::string::npos)
            return value;
        at = close;
    }
    return "";
}

std::vector<int> parts(const std::string& v) {
    std::vector<int> out;
    std::string cur;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur += c;
            if (cur.size() > 6) return {};        // not a version, a novel
        } else if (c == '.') {
            if (cur.empty()) return {};
            out.push_back(atoi(cur.c_str()));
            cur.clear();
        } else {
            return {};                            // letters, spaces: not a version
        }
    }
    if (cur.empty()) return {};
    out.push_back(atoi(cur.c_str()));
    return out.size() >= 2 && out.size() <= 4 ? out : std::vector<int>();
}

void work(std::string current) {
    Info info;
    info.checked = true;

    const std::string u = url();
    if (!safeUrl(u)) {
        info.error = "the update address is not one I will fetch";
    } else {
        std::string body, err;
        if (!fetch(u, &body, &err)) {
            info.error = err.empty() ? "could not reach the update site" : err;
        } else {
            const std::string tag = jsonString(body, "tag_name");
            info.latest = tag.empty() ? "" : (tag[0] == 'v' || tag[0] == 'V'
                                              ? tag.substr(1) : tag);
            info.url = jsonString(body, "html_url", "/releases");
            if (info.url.empty()) info.url = "github.com/Anviktor2411/xradio/releases";
            if (info.latest.empty()) info.error = "the update site said nothing useful";
            else info.newer = isNewer(current, info.latest);
        }
    }

    std::lock_guard<std::mutex> lk(g_mx);
    g_info = info;
}

}  // namespace

bool isNewer(const std::string& current, const std::string& candidate) {
    const std::vector<int> a = parts(current), b = parts(candidate);
    if (a.empty() || b.empty()) return false;
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (y > x) return true;
        if (y < x) return false;
    }
    return false;
}

void checkAsync(const std::string& current) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;   // once
    g_worker = std::thread(work, current);
}

Info latest() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_info;
}

void shutdown() {
    if (g_worker.joinable()) g_worker.join();
}

}  // namespace update
}  // namespace xr
