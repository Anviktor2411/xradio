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

# 4. min/max as bare macros break under <windows.h>; we define NOMINMAX, but
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
