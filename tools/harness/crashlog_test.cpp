// The crash handler (crashlog.h): an address is named as <module>+<offset>, a fault as a line Log.txt would
// carry, and -- on Windows, in a child process of this test -- a real access violation leaves "XRadio crash:
// access violation ... at crashlog_test.exe+0x..." with the faulting address and a stack, and the process still
// ends the way it would have without the handler.
//   ./crashlog_test                (the child is run as ./crashlog_test --crash <file>)
#include "crashlog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), !ok && !detail.empty() ? "   " : "", !ok && !detail.empty() ? detail.c_str() : "");
    if (!ok) ++fails;
}

static FILE* g_out = nullptr;
static void fileSink(const char* line) { if (g_out) { fputs(line, g_out); fflush(g_out); } }

static std::string readAll(const std::string& path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "--crash") == 0) {
        // the child: no error box, the handler to a file, then the fault
#ifdef _WIN32
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
        g_out = fopen(argv[2], "w");
        xr::crashlog::install(fileSink);
        xr::crashlog::crashNow();
        return 0;                              // never reached
    }

    const std::string here = xr::crashlog::describeAddress((const void*)&main);
    printf("this program's main() is at %s\n", here.c_str());
#ifdef _WIN32
    check(here.find("crashlog_test") != std::string::npos && here.find("+0x") != std::string::npos, "an address in this program is named <module>+<offset>: " + here);
    check(xr::crashlog::describeAddress(nullptr) == "0x0", "an address in no module is plain hex");
    const std::string fault = xr::crashlog::describeFault(0xC0000005u, (const void*)&main);
    check(fault.rfind("XRadio crash: access violation (0xc0000005) at crashlog_test", 0) == 0, "a fault line names the exception and the place: " + fault);
    check(xr::crashlog::install(fileSink), "the handlers install (dbghelp found)");
    xr::crashlog::uninstall();

    // the child process crashes for real
    char tmp[MAX_PATH] = {0};
    GetTempPathA(sizeof tmp, tmp);
    const std::string report = std::string(tmp) + "xradio-crashlog-test.txt";
    remove(report.c_str());
    const std::string cmd = std::string("\"\"") + argv[0] + "\" --crash \"" + report + "\"\"";
    const int rc = system(cmd.c_str());
    const std::string got = readAll(report);
    printf("child exit %d, report:\n%s", rc, got.c_str());
    check(rc != 0, "the child still died of its access violation (the handler only writes)");
    check(got.find("XRadio crash: access violation (0xc0000005) at crashlog_test") != std::string::npos, "the report names the fault and the module+offset");
    check(got.find("XRadio crash: writing address 0x10") != std::string::npos, "...and the address it tried to write");
    check(got.find("XRadio crash:   #0 crashlog_test") != std::string::npos, "...and a stack with the faulting frame first");
    check(got.find("XRadio crash:   #1 crashlog_test") != std::string::npos, "...unwound past it into the caller (no frame pointers needed)");
    check(got.find("unhandled: X-Plane is going down") != std::string::npos, "the last-chance filter wrote too, chained to the previous one");
    remove(report.c_str());
#else
    check(here.rfind("0x", 0) == 0, "no module names outside Windows: plain hex " + here);
    check(!xr::crashlog::install(fileSink), "nothing to install outside Windows");
    printf("SKIP  the crashing child: Windows only\n");
#endif
    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
