// The update check: the version comparison on its own, and the whole thing
// against a stand-in for GitHub (tools/harness/fake_release.py).
//
//   XRADIO_UPDATE_URL=http://127.0.0.1:5399/latest ./update_test <mode>
//
// A pilot on an old build cannot join a flight at all, so being told there
// is a newer one matters -- but a check that cries wolf, or that hangs the
// sim when the site is down, would be worse than none.
#include "update.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "offline";

    // "offline" means the update site cannot be reached. Left to itself this
    // test asked the real GitHub and then asserted that the answer was
    // useless -- so it passed only while the runner happened to be blocked or
    // rate-limited, and failed the day it got through. Point it at a port
    // with nothing behind it and the case is the one it claims to be.
    if (mode == "offline" && !getenv("XRADIO_UPDATE_URL")) {
#ifdef _WIN32
        _putenv_s("XRADIO_UPDATE_URL", "http://127.0.0.1:5398/nothing-here");
#else
        setenv("XRADIO_UPDATE_URL", "http://127.0.0.1:5398/nothing-here", 1);
#endif
    }

    printf("\ncomparing versions\n");
    {
        using xr::update::isNewer;
        check("a later patch is newer", isNewer("0.5.2", "0.5.3"));
        check("a later minor is newer", isNewer("0.5.9", "0.6.0"));
        check("ten beats nine, numerically not alphabetically",
              isNewer("0.5.9", "0.5.10"));
        check("the same version is not newer", !isNewer("0.5.2", "0.5.2"));
        check("an older one is not newer", !isNewer("0.5.2", "0.5.1"));
        check("a shorter version compares by what it has",
              isNewer("0.5", "0.5.1") && !isNewer("0.5.0", "0.5"));
        check("nonsense is never newer",
              !isNewer("0.5.2", "banana") && !isNewer("0.5.2", "") &&
              !isNewer("", "0.6.0") && !isNewer("0.5.2", "0.6.0-beta"));
        // A tag with a dot missing parses as a much larger version and beats
        // everything. This is not a bug in the comparison -- 05.4 really does
        // read as 5.4 -- it is why a release tag has to be exactly vX.Y.Z,
        // and it once told every pilot on 0.5.4 that "v05.4" was newer.
        check("a tag with a missing dot reads as a far higher version",
              isNewer("0.5.4", "05.4"));
        check("which the well-formed tag does not",
              !isNewer("0.5.4", "0.5.4"));
        check("absurd input is refused rather than parsed",
              !isNewer("0.5.2", "99999999.0.0"));
    }

    printf("\nasking the release site (%s)\n", mode.c_str());
    {
        const auto t0 = std::chrono::steady_clock::now();
        xr::update::checkAsync("0.5.2");
        xr::update::shutdown();               // waits for the worker
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const xr::update::Info i = xr::update::latest();
        printf("  (took %.1f s)  newer=%d latest='%s' url='%s' error='%s'\n",
               secs, (int)i.newer, i.latest.c_str(), i.url.c_str(), i.error.c_str());

        check("the check finished", i.checked);
        check("and did not hold anything up for long", secs < 8.0, std::to_string(secs));

        if (mode == "newer") {
            check("a newer version is noticed", i.newer);
            check("with its version number", i.latest == "9.9.9", i.latest);
            check("and a link to the release, not to the author",
                  i.url.find("/releases/") != std::string::npos, i.url);
        } else if (mode == "same") {
            // Assert what the site actually said, not only the verdict: if
            // the stand-in and this test ever disagree about what "the same
            // version" is, the case quietly becomes the "older" one and goes
            // on passing while testing nothing.
            check("the site really did offer the version we are running",
                  i.latest == "0.5.2", i.latest);
            check("no update is claimed", !i.newer, i.latest);
        } else if (mode == "older") {
            check("no update is claimed", !i.newer, i.latest);
        } else if (mode == "garbage" || mode == "error" || mode == "offline") {
            check("nothing is claimed when the answer is useless", !i.newer);
            check("and the reason is recorded", !i.error.empty(), i.error);
        } else if (mode == "huge") {
            // A megabyte of padding must not be swallowed whole.
            check("an oversized answer does not become an update claim",
                  !i.newer || i.latest == "9.9.9", i.latest);
        }
    }

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all update tests passed (%s)\n", mode.c_str());
    return 0;
}
