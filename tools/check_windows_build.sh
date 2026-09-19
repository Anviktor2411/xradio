#!/usr/bin/env bash
# Compile every source file for Windows, from Linux, in about twenty seconds.
#
# The Windows build is the one that breaks: code inside #ifdef _WIN32 is
# invisible to the Linux and macOS compilers, so a missing header or a
# mistyped Win32 call passes two of the three CI jobs and fails the third ten
# minutes later. SIO_UDP_CONNRESET did exactly that -- declared in
# <mstcpip.h>, not <winsock2.h>.
#
# MinGW is not MSVC, so this does not catch everything, but it does compile
# the Windows branches against the real Windows headers, which is where that
# whole class of mistake lives.
#
#   sudo apt-get install -y mingw-w64
#   bash tools/check_windows_build.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CXX=x86_64-w64-mingw32-g++
if ! command -v "$CXX" > /dev/null; then
    echo "  skip  $CXX not installed (apt-get install mingw-w64)"
    exit 0
fi

SDK="lib/XPMP2/lib/SDK"
if [ ! -d "$SDK/CHeaders/XPLM" ]; then
    SDK="tools/fake_sdk"
fi

FLAGS=(-std=c++17 -fsyntax-only -Wall -Wextra
       -DIBM=1 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS
       -DXPLM200=1 -DXPLM210=1 -DXPLM300=1 -DXPLM301=1 -DXPLM303=1 -DXPLM400=1
       -Iplugin/src "-I$SDK/CHeaders/XPLM" "-I$SDK/CHeaders/Widgets")

# Voice needs Opus and miniaudio; skip it rather than fail if they are absent.
if [ -f lib/miniaudio/miniaudio.h ] && [ -d lib/opus/include ]; then
    FLAGS+=(-DXRADIO_USE_VOICE=1 -Ilib/miniaudio -Ilib/opus/include)
    SOURCES="plugin/src/main.cpp plugin/src/net.cpp plugin/src/server.cpp
             plugin/src/ui.cpp plugin/src/upnp.cpp plugin/src/voice.cpp
             plugin/src/xpmp_bridge.cpp tools/server_main.cpp"
else
    echo "  note  Opus/miniaudio not fetched -- skipping voice.cpp"
    SOURCES="plugin/src/main.cpp plugin/src/net.cpp plugin/src/server.cpp
             plugin/src/ui.cpp plugin/src/upnp.cpp plugin/src/xpmp_bridge.cpp
             tools/server_main.cpp"
fi

fails=0
for f in $SOURCES; do
    if out=$("$CXX" "${FLAGS[@]}" "$f" 2>&1); then
        [ -n "$out" ] && { echo "  warn  $f"; echo "$out" | sed 's/^/          /'; }
        echo "  ok    $f"
    else
        echo "  FAIL  $f"
        echo "$out" | grep -E "error" | head -5 | sed 's/^/          /'
        fails=$((fails + 1))
    fi
done

echo
if [ "$fails" -gt 0 ]; then
    echo "$fails file(s) do not compile for Windows"
    exit 1
fi
echo "the whole plugin compiles for Windows"
