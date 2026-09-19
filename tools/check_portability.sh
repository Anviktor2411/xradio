#!/usr/bin/env bash
# Catches the portability mistakes that only show up on one platform's CI.
#
# Linux and macOS are forgiving about several things MSVC is not, so a change
# can pass two of the three builds and fail the third ten minutes later. These
# checks are instant and run before anything is compiled.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

fails=0
report() { echo "  FAIL  $1"; fails=$((fails + 1)); }
ok()     { echo "  ok    $1"; }

SOURCES=$(find plugin/src tools/harness -name '*.cpp' -o -name '*.h' | sort)

# 1. M_PI is POSIX, not standard C++. MSVC only defines it when
#    _USE_MATH_DEFINES precedes <cmath>, so its use depends on include order.
if hits=$(grep -ln "M_PI" $SOURCES 2>/dev/null | grep -v 'mathconst.h'); then
    report "M_PI used (not defined by MSVC's <cmath>) -- use xr::kPi from mathconst.h:"
    echo "$hits" | sed 's/^/          /'
else
    ok "no M_PI (portable xr::kPi used instead)"
fi

# 2. POSIX headers and functions with no MSVC equivalent.
posix='#include <unistd\.h>|#include <sys/socket\.h>|#include <netinet/|#include <arpa/|#include <sys/select\.h>|\busleep\(|\bstrcasecmp\(|\bstrndup\('
if hits=$(grep -lnE "$posix" $SOURCES 2>/dev/null); then
    # net.cpp is allowed: it guards every POSIX include behind #ifndef _WIN32
    unguarded=""
    for f in $hits; do
        grep -q "_WIN32" "$f" || unguarded="$unguarded $f"
    done
    if [ -n "$unguarded" ]; then
        report "POSIX-only API without a _WIN32 guard:$unguarded"
    else
        ok "POSIX APIs are all behind _WIN32 guards"
    fi
else
    ok "no POSIX-only APIs"
fi

# 3. Variable-length arrays: a GCC/Clang extension MSVC rejects outright.
if grep -nE '^\s+(char|int|float|double|uint8_t|int16_t)\s+\w+\[[a-z_][A-Za-z0-9_]*\]\s*;' $SOURCES 2>/dev/null \
     | grep -vE '\[(k[A-Z]|[A-Z_]+)' | grep -q .; then
    report "possible variable-length array (MSVC has no VLAs)"
else
    ok "no variable-length arrays"
fi

# 4. Windows APIs whose declaration lives in a header nobody remembers to
#    include. These compile everywhere else because the code sits inside
#    #ifdef _WIN32 and the other platforms never look at it -- so the mistake
#    survives Linux and macOS and only breaks the Windows build, ten minutes
#    in. SIO_UDP_CONNRESET did exactly that: it is in <mstcpip.h>, not
#    <winsock2.h>, and the run failed with "undeclared identifier".
#
#    One line per symbol: <symbol> <header that declares it>
win_needs='
SIO_UDP_CONNRESET mstcpip.h
SIO_LOOPBACK_FAST_PATH mstcpip.h
WSAIoctl winsock2.h
WSAStartup winsock2.h
closesocket winsock2.h
ioctlsocket winsock2.h
getnameinfo ws2tcpip.h
getaddrinfo ws2tcpip.h
freeaddrinfo ws2tcpip.h
inet_ntop ws2tcpip.h
NI_MAXHOST ws2tcpip.h
NI_MAXSERV ws2tcpip.h
IP_MULTICAST_TTL ws2tcpip.h
'
win_bad=""
while read -r sym hdr; do
    [ -z "$sym" ] && continue
    for f in $(grep -lw "$sym" $SOURCES 2>/dev/null); do
        # Only files that compile Windows-specific code can be at fault.
        grep -q "_WIN32" "$f" || continue
        grep -q "#  *include <$hdr>" "$f" || win_bad="$win_bad
          $f uses $sym but does not include <$hdr>"
    done
done <<< "$win_needs"
if [ -n "$win_bad" ]; then
    report "Windows API used without the header that declares it:$win_bad"
else
    ok "Windows APIs have their declaring headers"
fi

# 5. min/max as bare macros break under <windows.h>; we define NOMINMAX, but
#    catch anyone reaching for the C++ versions without <algorithm>.
algo_bad=""
for f in $(grep -l 'std::min\|std::max' $SOURCES 2>/dev/null); do
    grep -q '#include <algorithm>' "$f" || algo_bad="$algo_bad $f"
done
if [ -n "$algo_bad" ]; then
    report "std::min/max without <algorithm>:$algo_bad"
else
    ok "std::min/max users include <algorithm>"
fi

echo
if [ "$fails" -gt 0 ]; then
    echo "$fails portability problem(s)"
    exit 1
fi
echo "portability checks passed"
