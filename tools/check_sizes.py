#!/usr/bin/env python3
"""The Python half of the wire-format check, and the comparison itself.

`plugin/src/protocol.h` and `server/protocol.py` describe the same bytes twice,
so the only thing keeping them honest is comparing what each one thinks those
bytes are. `tools/check_sizes.cpp` prints the C++ answers; this prints the
Python ones in the same order, and with --compare it runs the C++ side and
diffs the two itself.

    python3 tools/check_sizes.py                      # just print
    python3 tools/check_sizes.py --compare ./check_sizes

The comparison lives here rather than in a shell pipeline in the CI workflow
because a list kept in a YAML heredoc cannot be run anywhere else: a constant
added to one side and not to that heredoc passes locally and fails in CI ten
minutes later, which is exactly how GUARD_KHZ got through.
"""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "server"))

import protocol as P          # noqa: E402

# Every struct whose layout both sides have to agree on.
STRUCTS = ("HEADER", "LOGIN", "LOGIN_ACK", "LOGIN_REJECT", "POSITION",
           "TRAFFIC_HDR", "TRAFFIC_ENTRY", "TEXT_HDR", "VOICE_HDR", "WEATHER")

# Not structs, but the same class of mistake: a packet budget, an entry cap or
# a well-known frequency that drifts apart shows up as traffic that thins out
# for the clients of one server, or a frequency that is special on one side.
CONSTANTS = (
    ("MAX_PACKET", lambda: P.MAX_PACKET),
    ("MAX_TRAFFIC_ENTRIES", lambda: P.MAX_TRAFFIC_ENTRIES),
    ("GUARD_KHZ", lambda: P.GUARD_KHZ),
)


def lines():
    out = [f"{n} {getattr(P, n).size}" for n in STRUCTS]
    out += [f"{n} {fn()}" for n, fn in CONSTANTS]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compare", metavar="EXE",
                    help="run the compiled check_sizes and diff it against this")
    args = ap.parse_args()

    mine = lines()
    if not args.compare:
        print("\n".join(mine))
        return 0

    try:
        p = subprocess.run([args.compare], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as exc:
        print(f"could not run {args.compare}: {exc}")
        return 2
    if p.returncode != 0:
        print(f"{args.compare} exited {p.returncode}")
        print(p.stdout + p.stderr)
        return 2

    theirs = [l.rstrip() for l in p.stdout.strip().splitlines() if l.strip()]
    if theirs == mine:
        print(f"the {len(mine)} wire values agree")
        return 0

    print("the C++ and Python wire formats disagree:")
    a = dict(l.split(" ", 1) for l in theirs if " " in l)
    b = dict(l.split(" ", 1) for l in mine if " " in l)
    for name in sorted(set(a) | set(b)):
        x, y = a.get(name), b.get(name)
        if x == y:
            continue
        if x is None:
            print(f"  {name}: only in server/protocol.py ({y})"
                  f"  -- add it to tools/check_sizes.cpp")
        elif y is None:
            print(f"  {name}: only in plugin/src/protocol.h ({x})"
                  f"  -- add it to tools/check_sizes.py")
        else:
            print(f"  {name}: C++ says {x}, Python says {y}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
