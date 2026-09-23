#!/usr/bin/env bash
# Compile the CSL bridge against the real XPMP2 headers, for Linux and Windows.
#
# The test build turns XPMP2 off -- the stub SDK has no renderer to talk to --
# so everything inside `#ifdef XRADIO_USE_XPMP2` is invisible to every test in
# this repository. That is the code that actually draws other aircraft: get it
# wrong and the tests still pass, the plugin still builds here, and the mistake
# only turns up in somebody's sim.
#
# This compiles it the way the shipped plugin compiles it. It is a syntax and
# type check, not a link, so it takes seconds.
#
#   bash tools/check_xpmp2_build.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SDK="lib/XPMP2/lib/SDK"
if [ ! -d "$SDK/CHeaders/XPLM" ] || [ ! -d lib/XPMP2/inc ]; then
    echo "  skip  XPMP2 not fetched (bash tools/fetch_deps.sh)"
    exit 0
fi

SOURCES="plugin/src/xpmp_bridge.cpp"
COMMON=(-std=c++17 -fsyntax-only -Wall -Wextra
        -DXRADIO_USE_XPMP2=1
        -DXPLM200=1 -DXPLM210=1 -DXPLM300=1 -DXPLM301=1 -DXPLM303=1 -DXPLM400=1
        -Iplugin/src -Ilib/XPMP2/inc
        "-I$SDK/CHeaders/XPLM" "-I$SDK/CHeaders/Widgets")

fails=0

try() {                     # try <label> <compiler> <platform-define>...
    local label="$1" cxx="$2"; shift 2
    if ! command -v "$cxx" > /dev/null; then
        echo "  skip  $label ($cxx not installed)"
        return
    fi
    for f in $SOURCES; do
        if out=$("$cxx" "${COMMON[@]}" "$@" "$f" 2>&1); then
            [ -n "$out" ] && { echo "  warn  $label: $f"; echo "$out" | sed 's/^/          /'; }
            echo "  ok    $label: $f"
        else
            echo "  FAIL  $label: $f"
            echo "$out" | grep -E "error" | head -8 | sed 's/^/          /'
            fails=$((fails + 1))
        fi
    done
}

try "linux"   g++                    -DLIN=1
try "windows" x86_64-w64-mingw32-g++ -DIBM=1 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS

echo
if [ "$fails" -gt 0 ]; then
    echo "$fails failed"
    exit 1
fi
echo "the CSL bridge compiles against real XPMP2"
