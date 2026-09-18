#!/usr/bin/env bash
# Fetches XPMP2 into lib/XPMP2. XPMP2 ships a complete X-Plane SDK in
# lib/XPMP2/lib/SDK, so this is the only dependency you need to build.
#
# Usage:  ./tools/fetch_deps.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XPMP2_DIR="$ROOT/lib/XPMP2"
XPMP2_TAG="${XPMP2_TAG:-v3.6.1}"

mkdir -p "$ROOT/lib"

if [ -d "$XPMP2_DIR/.git" ]; then
    echo "XPMP2 already present in lib/XPMP2 -- updating to $XPMP2_TAG"
    git -C "$XPMP2_DIR" fetch --tags --depth 1 origin "$XPMP2_TAG"
    git -C "$XPMP2_DIR" checkout -q "$XPMP2_TAG"
else
    echo "Cloning XPMP2 $XPMP2_TAG into lib/XPMP2"
    git clone --depth 1 --branch "$XPMP2_TAG" \
        https://github.com/TwinFan/XPMP2.git "$XPMP2_DIR"
fi

if [ ! -f "$XPMP2_DIR/lib/SDK/CHeaders/XPLM/XPLMPlugin.h" ]; then
    echo "WARNING: no X-Plane SDK inside XPMP2. Download it from"
    echo "         https://developer.x-plane.com/sdk/plugin-sdk-downloads/"
    echo "         and configure with -DXPLANE_SDK=<path>."
fi

echo
echo "Done. Now build with:"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build -j"
