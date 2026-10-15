#!/usr/bin/env bash
# SimBrief import end to end without X-Plane or a SimBrief account: expects build-tests/simbrief_test, starts
# tools/fake_simbrief.py (the official fetch endpoint's shape, HTTP 400 for an unknown user) on a spare port,
# runs the test against it through the plugin's own HTTP client, stops it.
#   bash tools/check_simbrief.sh [build dir]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="${1:-build-tests}"
EXE="$BUILD/simbrief_test"
[ -x "$EXE" ] || EXE="$EXE.exe"
[ -x "$EXE" ] || { echo "no $BUILD/simbrief_test -- configure with -DXRADIO_BUILD_TESTS=ON and build the simbrief_test target"; exit 1; }
PORT=48790
python3 tools/fake_simbrief.py --port "$PORT" --fixture tools/fixtures/simbrief_ofp.json > "${TMPDIR:-/tmp}/xradio-fake-simbrief-$$.log" 2>&1 &
FAKE=$!
trap 'kill $FAKE 2>/dev/null || true' EXIT
for i in $(seq 1 50); do
    if python3 - "$PORT" <<'PY'
import socket, sys
s = socket.socket(); s.settimeout(0.2)
try:
    s.connect(("127.0.0.1", int(sys.argv[1]))); s.close(); sys.exit(0)
except OSError:
    sys.exit(1)
PY
    then break; fi
    python3 -c "import time; time.sleep(0.2)"
done
XR_SIMBRIEF_PORT="$PORT" "$EXE" tools/fixtures/simbrief_ofp.json
