#!/usr/bin/env bash
# Wait until something is listening on a local TCP port, then return.
#
#   bash tools/harness/wait_port.sh <port> [seconds] [pid]
#
# CI used to start a helper server, `sleep 1`, and hope. On a loaded runner
# that is not always long enough, and the test that follows then measures the
# helper's absence instead of the thing under test -- a failure that looks
# like a real bug and cannot be reproduced anywhere.
#
# The check is a bare TCP connect through bash's own /dev/tcp, deliberately
# not curl: an environment with a proxy in the environment has curl answering
# for a port nothing is listening on, which would make this wait succeed
# instantly and silently put the race back.
#
# Give it the helper's pid as a third argument and it also gives up the moment
# that process dies -- which is what happens when the port is still held by
# the previous test's server and the new one cannot bind.
set -u

port="${1:?usage: wait_port.sh <port> [seconds] [pid]}"
limit="${2:-20}"
pid="${3:-}"
deadline=$(( $(date +%s) + limit ))

while [ "$(date +%s)" -lt "$deadline" ]; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        exec 3<&- 3>&- 2>/dev/null
        exit 0
    fi
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
        echo "the server on port $port exited before it was listening" >&2
        exit 1
    fi
    sleep 0.2
done

echo "nothing is listening on port $port after ${limit}s" >&2
exit 1
