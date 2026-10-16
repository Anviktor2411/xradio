#!/usr/bin/env python3
"""The Python half of the wire-format check, and the comparison itself.

`plugin/src/protocol.h` and `server/protocol.py` describe the same bytes twice,
so the only thing keeping them honest is comparing what each one thinks those
bytes are. `tools/check_sizes.cpp` prints the C++ answers; this prints the
Python ones in the same order, and with --compare it runs the C++ side and
diffs the two itself.

The radio model is held to the same: `plugin/src/radio.h` and `server/server.py`
both work out a table of links (an apron, a final, FL350 to a tower, a centre's
area, a duct...) and the answers have to match to the printed digit -- the two
servers relay by them.

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
import server as S            # noqa: E402

# Every struct whose layout both sides have to agree on.
STRUCTS = ("HEADER", "LOGIN", "LOGIN_ACK", "LOGIN_REJECT", "POSITION",
           "TRAFFIC_HDR", "TRAFFIC_ENTRY", "TEXT_HDR", "VOICE_HDR", "WEATHER",
           "ROSTER_HDR", "ROSTER_ENTRY", "FLIGHTPLAN", "ATIS_HDR", "ATC_LIST_HDR", "ATC_ENTRY",
           "AIRCRAFT", "TCAS")

# Not structs, but the same class of mistake: a packet budget, an entry cap or
# a well-known frequency that drifts apart shows up as traffic that thins out
# for the clients of one server, or a frequency that is special on one side.
CONSTANTS = (
    ("MAX_PACKET", lambda: P.MAX_PACKET),
    ("MAX_TRAFFIC_ENTRIES", lambda: P.MAX_TRAFFIC_ENTRIES),
    ("MAX_ROSTER_ENTRIES", lambda: P.MAX_ROSTER_ENTRIES),
    ("PT_ROSTER", lambda: P.PT_ROSTER),
    ("GUARD_KHZ", lambda: P.GUARD_KHZ),
    ("PROTO_VERSION", lambda: P.PROTO_VERSION),
    ("MAX_ATC_ENTRIES", lambda: P.MAX_ATC_ENTRIES),
    ("MAX_ATIS_BYTES", lambda: P.MAX_ATIS_BYTES),
    ("PT_AI_TRAFFIC", lambda: P.PT_AI_TRAFFIC),
    ("PT_FLIGHTPLAN", lambda: P.PT_FLIGHTPLAN),
    ("PT_ATIS", lambda: P.PT_ATIS),
    ("PT_ATC_LIST", lambda: P.PT_ATC_LIST),
    ("PT_AIRCRAFT", lambda: P.PT_AIRCRAFT),
    ("PT_TCAS", lambda: P.PT_TCAS),
    ("LF_ATC", lambda: P.LF_ATC),
    ("LF_INJECTOR", lambda: P.LF_INJECTOR),
    ("AI_SID", lambda: P.ai_sid(7, 12)),
    ("MIN_PROTO_VERSION", lambda: P.MIN_PROTO_VERSION),
    ("POSITION_V5", lambda: P.POSITION_V5.size),
    ("TRAFFIC_ENTRY_V5", lambda: P.TRAFFIC_ENTRY_V5.size),
    ("MAX_TRAFFIC_ENTRIES_V5", lambda: P.MAX_TRAFFIC_ENTRIES_V5),
    ("AL_DUCTING", lambda: P.AL_DUCTING),
    ("AE_POSITION", lambda: P.AE_POSITION),
)


def _aircraft(lat, lon, alt_msl_m, agl_m):
    return S.Node(lat, lon, alt_msl_m, S.aircraft_height_ft(agl_m))


def _station(lat, lon, alt_msl_m, kind):
    ant, rng, ceil = S.STATION_DEFAULTS.get(kind, S.STATION_UNKNOWN)
    return S.Node(lat, lon, alt_msl_m, ant * S.FT_PER_M, True, rng, ceil)


def _radio(name, tx, rx, khz, ducting):
    q, level, dist, cov = S.link(tx, rx, khz, ducting)
    return f"RADIO_{name} {q:.4f} {level:.2f} {dist:.3f} {cov:.3f}"


def _warm_still(date_days, zulu, temp_c=24.0):
    """A WEATHER tuple as the server unpacks one: warm, high pressure, calm, dry."""
    w = [0, zulu, date_days, 0.0, 102200.0, temp_c, 0.0, 0.0, 0.0, 0] + [0.0] * (P.WEATHER.size // 4 - 10)
    w[35] = 2.5                                  # windSpeedMs[0]
    return P.WEATHER.unpack(P.WEATHER.pack(*w))  # through float32, as on the wire


def radio_cases():
    """The cases tools/check_sizes.cpp prints, in its order."""
    lowi_a = _aircraft(47.2602, 11.3439, 581.0, 0.0)
    twr = _station(59.4133, 24.8328, 70.0, P.ATC_TWR)
    fin = _aircraft(59.4133, 24.5054, 914.4, 873.0)
    hi200 = _aircraft(62.7466, 24.8328, 10668.0, 10600.0)
    sea_a, sea_b = _aircraft(59.60, 22.00, 152.4, 152.4), _aircraft(59.60, 24.6350, 152.4, 152.4)
    out = [
        _radio("APRON_LOWI", lowi_a, _aircraft(47.3002, 11.3439, 581.0, 0.0), 120100, False),
        _radio("APRON_LOWI_10NM", lowi_a, _aircraft(47.4269, 11.3439, 581.0, 0.0), 120100, False),
        _radio("APRON_LOWI_V5", lowi_a,
               _aircraft(47.3002, 11.3439, 581.0, S.effective_agl_m(False, 0.0, True, 581.0)), 120100, False),
        _radio("V5_AIRBORNE", _aircraft(57.85, 27.02, 914.4, S.effective_agl_m(False, 0.0, False, 914.4)),
               _aircraft(59.50, 27.00, 914.4, S.effective_agl_m(False, 0.0, False, 914.4)), 122800, False),
        _radio("TWR_TO_FINAL", twr, fin, 118100, False),
        _radio("FINAL_TO_TWR", fin, twr, 118100, False),
        _radio("TWR_SERVICE_VOLUME", twr, _aircraft(59.7466, 24.8328, 40.0, 0.0), 118100, False),
        _radio("TWR_ABOVE_CEILING", twr, _aircraft(59.7466, 24.8328, 3000.0, 2960.0), 118100, False),
        _radio("FL350_TO_TWR_200NM", hi200, twr, 132800, False),
        _radio("TWR_TO_FL350_200NM", twr, hi200, 132800, False),
        _radio("FL350_TO_TWR_260NM", _aircraft(63.7466, 24.8328, 10668.0, 10600.0), twr, 132800, False),
        _radio("CTR_FL100_180NM", _station(59.4133, 24.8328, 70.0, P.ATC_CTR),
               _aircraft(62.4133, 24.8328, 3048.0, 3000.0), 132800, False),
        _radio("SEA_80NM", sea_a, sea_b, 121500, False),
        _radio("SEA_80NM_DUCT", sea_a, sea_b, 121500, True),
    ]
    for zulu in (0.0, 3600.0 * 7, 3600.0 * 18, 3600.0 * 21):
        out.append(f"DUCTING_{int(zulu // 3600)} {1 if S.ducting_from(_warm_still(19500, zulu)) else 0}")
    out.append(f"DUCTING_COLD {1 if S.ducting_from(_warm_still(19500, 3600.0 * 18, 8.0)) else 0}")
    return out


def lines():
    out = [f"{n} {getattr(P, n).size}" for n in STRUCTS]
    out += [f"{n} {fn()}" for n, fn in CONSTANTS]
    out += radio_cases()
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
