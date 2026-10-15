#!/usr/bin/env bash
# Tower mode end to end without X-Plane: builds nothing, expects build-tests/tower_test (see README "Testing
# without X-Plane"), starts tools/fake_towertrainer.py on a spare port, runs the test against it, stops it.
#   bash tools/check_tower.sh [build dir]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="${1:-build-tests}"
EXE="$BUILD/tower_test"
[ -x "$EXE" ] || EXE="$EXE.exe"
[ -x "$EXE" ] || { echo "no $BUILD/tower_test -- configure with -DXRADIO_BUILD_TESTS=ON and build the tower_test target"; exit 1; }
PORT=48781
LOG="${TMPDIR:-/tmp}/xradio-tower-rx-$$.txt"
rm -f "$LOG"
python3 tools/fake_towertrainer.py --port "$PORT" --log "$LOG" > "${TMPDIR:-/tmp}/xradio-fake-tower-$$.log" 2>&1 &
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
XR_TOWER_PORT="$PORT" XR_TOWER_LOG="$LOG" "$EXE"
rc=$?
echo "--- what the fake trainer received ---"
cat "$LOG" 2>/dev/null | head -20
exit $rc
