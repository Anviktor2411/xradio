#!/usr/bin/env bash
# Fetches the third-party code the plugin builds against, into lib/:
#   XPMP2      -- CSL models, TCAS, map (also ships a complete X-Plane SDK)
#   opus       -- the voice codec
#   miniaudio  -- microphone and speaker access, one header
#
# Usage:  ./tools/fetch_deps.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XPMP2_TAG="${XPMP2_TAG:-v3.6.1}"
OPUS_TAG="${OPUS_TAG:-v1.5.2}"
MINIAUDIO_TAG="${MINIAUDIO_TAG:-0.11.25}"

mkdir -p "$ROOT/lib"

fetch() {   # fetch <name> <url> <tag>
    local dir="$ROOT/lib/$1"
    if [ -d "$dir/.git" ]; then
        echo "$1: already present, updating to $3"
        git -C "$dir" fetch --tags --depth 1 origin "$3"
        git -C "$dir" checkout -q "$3"
    else
        echo "$1: cloning $3"
        git clone --depth 1 --branch "$3" "$2" "$dir"
    fi
}

fetch XPMP2     https://github.com/TwinFan/XPMP2.git    "$XPMP2_TAG"
fetch opus      https://github.com/xiph/opus.git        "$OPUS_TAG"
fetch miniaudio https://github.com/mackron/miniaudio.git "$MINIAUDIO_TAG"

if [ ! -f "$ROOT/lib/XPMP2/lib/SDK/CHeaders/XPLM/XPLMPlugin.h" ]; then
    echo "WARNING: no X-Plane SDK inside XPMP2. Download it from"
    echo "         https://developer.x-plane.com/sdk/plugin-sdk-downloads/"
    echo "         and configure with -DXPLANE_SDK=<path>."
fi

echo
echo "Done. Now build with:"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build -j"
