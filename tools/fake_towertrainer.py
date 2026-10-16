#!/usr/bin/env python3
"""A stand-in for TowerTrainer's tower link (server.py --tower-port), for the plugin's Tower-mode tests.

It speaks the same newline-separated JSON: after the plugin's hello it sends "link", "welcome", "airport"
(a made-up airport shaped like a real one: runway 08/26, a parallel taxiway with links, two aprons with
stands, holding points, the taxi network and the pavement) and then a "state" four times a second (eleven
aircraft in every phase, one an incoming handoff, one asking for its clearance, one a live pilot, with the
Delivery list and the log in the trainer's own format), answers "claim" with "claims", "ping" with "pong",
moves a strip to the bay it is told, and writes every line it receives to --log so a test can check what
the plugin sent (claim, tx, strip accept / bay, ops).

    python tools/fake_towertrainer.py --port 8781 --log received.txt
    python tools/fake_towertrainer.py --dump tools/fixtures/tower_preview.jsonl   # the airport and one state, for
                                                                                  # tools/ui_preview and tower_ui_test
"""
import argparse
import json
import socket
import threading
import time


def _airport():
    """Not a real layout -- nothing from X-Plane's scenery is copied here -- but laid out like one, in the
    trainer's local metres (east, north) round the datum."""
    nodes, edges, names = [], [], {}

    def node(key, x, y):
        names[key] = len(nodes)
        nodes.append([float(x), float(y)])

    def edge(a, b, name, width):
        edges.append([names[a], names[b], name, float(width)])

    # parallel taxiway A south of the runway, links A1..A5 to the holding points
    xs = {"A5": -1450, "A4": -500, "A3": 300, "A2": 1000, "A1": 1900}
    for k, x in xs.items():
        node(k + "a", x, -180)          # on taxiway A
        node(k + "h", x, -60)           # at the holding point
    order = ["A5", "A4", "A3", "A2", "A1"]
    for a, b in zip(order, order[1:]):
        edge(a + "a", b + "a", "A", 23)
    for k in order:
        edge(k + "a", k + "h", k, 23)
    # the main apron's two taxiways, the cargo apron's one
    node("Ba", 900, -180); node("Bp", 900, -270)
    node("Ca", 1250, -180); node("Cp", 1250, -270)
    node("Da", -120, -180); node("Dp", -120, -270)
    edges.append([names["A2a"], names["Ba"], "A", 23.0]); edges.append([names["Ba"], names["Ca"], "A", 23.0])
    edge("Ba", "Bp", "B", 18); edge("Ca", "Cp", "C", 18); edge("Da", "Dp", "D", 15)
    stands = [{"name": f"K{i + 1}", "x": round(981.6 + (i if i < 5 else 4 - i) * 55.0, 1), "y": -325.0, "hdg": 180} for i in range(8)]
    stands += [{"name": f"C{i + 1}", "x": -260.0 + i * 70.0, "y": -330.0, "hdg": 180} for i in range(3)]
    holds = [{"id": i, "x": float(xs[k]), "y": -60.0} for i, k in enumerate(order)]
    pavement = [[[[760.0, -265.0], [1290.0, -265.0], [1290.0, -400.0], [760.0, -400.0]]],
                [[[-310.0, -265.0], [-60.0, -265.0], [-60.0, -390.0], [-310.0, -390.0]], [[-200.0, -330.0], [-180.0, -330.0], [-180.0, -310.0], [-200.0, -310.0]]]]
    return {"type": "airport", "protocol": 1, "icao": "EETN", "name": "Lennart Meri Tallinn", "lat": 59.413333, "lon": 24.8325, "elev_ft": 131,
            "tower": [600.0, -250.0],
            "runways": [{"name": "08/26", "width": 45, "ends": [{"name": "08", "x": -1515.9, "y": 0.08, "displaced": 248}, {"name": "26", "x": 1965.6, "y": -17.8, "displaced": 0}]}],
            "holdLines": holds, "stands": stands, "taxi": {"nodes": nodes, "edges": edges}, "pavement": pavement}


AIRPORT = _airport()
POS = {k: {"key": k, "cs": f"EETN_{k}" if k != "CTR" else "EE_CTR", "name": n, "freq": f, "mhz": float(f), "unit": u} for k, n, f, u in
       [("DEL", "Tallinn Delivery", "121.805", "Delivery"), ("GND", "Tallinn Ground", "131.905", "Ground"), ("TWR", "Tallinn Tower", "135.905", "Tower"),
        ("APP", "Tallinn Radar", "127.905", "Approach"), ("CTR", "Tallinn Control", "132.805", "Control")]}


def _ac(i, cs, tel, num, typ, wtc, kind, state, x, y, alt, hdg, spd, freq, squawk, other, bays, **more):
    a = {"id": i, "cs": cs, "tel": tel, "num": num, "type": typ, "wtc": wtc, "kind": kind, "state": state, "pos": [round(x, 1), round(y, 1)], "alt": round(alt),
         "hdg": hdg, "spd": spd, "onGround": alt < 200, "freq": freq, "squawk": squawk, "assignedSq": "", "other": other, "handoff": None, "stand": "", "rwy": "26",
         "sid": "", "cfl": 0, "strip": bays.get(i), "warn": [], "waiting": None, "callingUntil": 0}
    a.update(more)
    return a


def state(t, claims, my_id, bays):
    now = int(time.time() * 1000)
    ac = [
        _ac(1, "FIN321", "Finnair", "three two one", "A320", "M", "dep", "holding", 1900.0, -40.0, 131, 260, 0, "TWR", "4521", ["EFHK", "Helsinki"], bays,
            assignedSq="4521", sid="ULGOT1G", cfl=4000, callingUntil=now + 2500),
        _ac(2, "SAS123", "Scandinavian", "one two three", "B738", "M", "dep", "taxi", 400.0 + t * 5.0, -180.0, 131, 90, 8.0, "TWR", "4522", ["ESSA", "Stockholm"], bays,
            assignedSq="4522", handoff={"from": "GND", "to": "TWR", "accepted": False, "age": int(t)}),
        _ac(3, "DLH5KM", "Lufthansa", "five kilo mike", "A321", "M", "arr", "final", 7600.0 - t * 70.0, -40.0, max(600.0, 1400 - t * 15), 260, 72.0, "TWR", "4530",
            ["EDDF", "Frankfurt"], bays),
        _ac(4, "BTI4KT", "Air Baltic", "four kilo tango", "BCS3", "M", "dep", "clr_req", 981.6, -325.0, 131, 180, 0, "DEL", "2000", ["EVRA", "Riga"], bays,
            stand="K1", waiting=int(t) + 64),
        _ac(5, "RYR8VX", "Ryanair", "eight victor x-ray", "B738", "M", "dep", "cleared", 1091.6, -325.0, 131, 180, 0, "DEL", "4525", ["EGSS", "London Stansted"], bays,
            stand="K3", assignedSq="4525", sid="TLL1D", cfl=4000),
        _ac(6, "AFR1752", "Air France", "one seven five two", "A319", "M", "arr", "vacating", 1000.0, -105.0, 131, 180, 9.0, "TWR", "4531", ["LFPG", "Paris"], bays),
        _ac(7, "ESXYZ", "ESXYZ", "", "C172", "L", "dep", "taxi", 900.0, -228.0, 131, 0, 4.0, "GND", "7000", ["EVRA", "Riga"], bays, human=True),
        _ac(8, "FOLLOW1", "Follow me one", "", "", "", "veh", "leading", 1180.0, -292.0, 131, 270, 3.0, "GND", "", ["", ""], bays, veh=True, vtype="follow-me"),
        _ac(9, "LOT789", "Lot", "seven eight niner", "E75L", "M", "dep", "lined_up", 1930.0, -17.0, 131, 270, 0, "TWR", "4526", ["EPWA", "Warsaw"], bays,
            assignedSq="4526", sid="ULGOT1G", cfl=4000),
        _ac(10, "KLM1132", "KLM", "one one three two", "B737", "M", "arr", "final", 15000.0 - t * 75.0, -60.0, 3000, 262, 82.0, "APP", "4532", ["EHAM", "Amsterdam"], bays),
        _ac(11, "NOZ51", "Nordic", "five one", "B38M", "M", "dep", "airborne", -3600.0 - t * 80.0, 120.0, 2400 + t * 20, 262, 90.0, "TWR", "4527", ["ENGM", "Oslo"], bays,
            assignedSq="4527", sid="TLL1D", cfl=4000, warn=["SQK 4527"] if t < 5 else []),
        # a busy apron: parked aircraft (one parked for the night) and service vehicles, for the radar's label filter
        _ac(12, "EZY12AB", "Easy", "one two alpha bravo", "A319", "M", "dep", "parked", 1036.6, -325.0, 131, 180, 0, "DEL", "2000", ["EGKK", "London Gatwick"], bays, stand="K2"),
        _ac(13, "NSZ4UV", "Rednose", "four uniform victor", "B38M", "M", "dep", "parked", 1146.6, -325.0, 131, 180, 0, "DEL", "2000", ["ENBR", "Bergen"], bays, stand="K4"),
        _ac(14, "TOM7PL", "Tomson", "seven papa lima", "B738", "M", "dep", "parked", 1201.6, -325.0, 131, 180, 0, "DEL", "2000", ["EGCC", "Manchester"], bays, stand="K5"),
        _ac(15, "WZZ3MX", "Wizz Air", "three mike x-ray", "A21N", "M", "dep", "parked", 926.6, -325.0, 131, 180, 0, "DEL", "2000", ["EPKT", "Katowice"], bays, stand="K6"),
        _ac(16, "CLX74K", "Cargolux", "seven four kilo", "B748", "H", "dep", "parked", 871.6, -325.0, 131, 180, 0, "DEL", "2000", ["ELLX", "Luxembourg"], bays, stand="K7",
            dormant=True),
        _ac(17, "BUS1", "", "", "", "", "veh", "roam", 1000.0, -362.0, 131, 90, 0, "", "", ["", ""], bays, veh=True, vtype="bus", quiet=True),
        _ac(18, "FUEL2", "", "", "", "", "veh", "roam", 1120.0, -352.0, 131, 270, 0, "", "", ["", ""], bays, veh=True, vtype="fuel", quiet=True),
        _ac(19, "BAGGAGE3", "", "", "", "", "veh", "roam", 1062.0, -356.0, 131, 0, 0, "", "", ["", ""], bays, veh=True, vtype="baggage", quiet=True),
    ]
    delivery = [
        {"id": 7, "cs": "ESXYZ", "live": True, "type": "C172", "wtc": "L", "dest": "EVRA", "destName": "", "stand": "", "sid": "", "cfl": "4500", "squawk": "4524",
         "waiting": int(t) + 20, "cleared": False, "clearance": "ESXYZ cleared to Riga via flight planned route, climb 4500 feet, squawk 4524", "note": ""},
        {"id": 4, "cs": "BTI4KT", "live": False, "type": "BCS3", "wtc": "M", "dest": "EVRA", "destName": "Riga", "stand": "K1", "sid": "TLL1D", "cfl": "4000",
         "squawk": "4523", "waiting": int(t) + 64, "cleared": False,
         "clearance": "Air Baltic four kilo tango cleared to Riga via TLL1D departure, climb 4000 feet, squawk 4523", "note": ""},
        {"id": 5, "cs": "RYR8VX", "live": False, "type": "B738", "wtc": "M", "dest": "EGSS", "destName": "London Stansted", "stand": "K3", "sid": "TLL1D", "cfl": "4000",
         "squawk": "4525", "waiting": -1, "cleared": True, "clearance": "", "note": ""},
    ]
    log = [("sys", "0:01", "EETN · runway 26"),
           ("pilot arr", "0:03", "<b>AFR1752</b> Tallinn Tower, Air France one seven five two, vacated runway 26 via A2"),
           ("atc", "0:03", "<b>EETN_TWR</b> Air France one seven five two, contact Ground 131.905, good day"),
           ("pilot dep", "0:04", "<b>FIN321</b> Tallinn Tower, Finnair three two one, holding point runway 26, ready for departure"),
           ("atc", "0:06", "<b>EETN_TWR</b> Finnair three two one, hold position"),
           ("pilot dep", "0:07", "<b>FIN321</b> Holding position, Finnair three two one"),
           ("pilot arr", "0:09", "<b>DLH5KM</b> Tallinn Tower, Lufthansa five kilo mike, ILS runway 26, 4 miles"),
           ("ai", "0:10", "<b>EETN_GND (AI)</b> \U0001F4A1 SAS123 is coming to you on A"),
           ("sys", "0:11", "✓ strip LOT789 accepted by X-Plane")]
    return {"type": "state", "icao": "EETN", "t": f"{int(t) // 60}:{int(t) % 60:02d}", "ac": ac, "split": True, "positions": POS, "delivery": delivery, "stopBars": True,
            "staffed": ["TWR"] + (["TWR"] if claims.get(my_id) == "TWR" else []), "selectedId": None, "rwy": "26", "arrEnd": "26",
            "atis": "<b>C</b> <span class=\"raw\">METAR EETN 150620Z 20006KT 9999 OVC007 09/07 Q1022</span> Tallinn information Charlie, runway 26 in use, ILS approach, "
                    "wind 200 degrees 6 knots, visibility 10 kilometres, overcast 700 feet, QNH 1022",
            "closed": [], "lvp": False, "seq": "", "wx": "", "del": "", "seqOn": False, "chat": [], "metar": "METAR EETN 150620Z 20006KT 9999 OVC007 09/07 Q1022",
            "claims": claims, "roster": [{"id": my_id, "name": "X-Plane", "role": "guest", "pos": claims.get(my_id, "")}], "coords": [],
            "remoteStaff": {claims[my_id]: "X-Plane"} if claims.get(my_id) else {}, "hostNick": "viktor", "utc": now,
            "logs": {"TWR": [{"cls": c, "html": f'<span class="t">{tm}</span>{h}'} for c, tm, h in log]}}


def serve(conn, addr, log):
    f = conn.makefile("rwb")
    claims = {}
    bays = {}            # strip id -> {"bay", "state"}: where the controller moved it
    my_id = "x1"
    lock = threading.Lock()

    def send(obj):
        with lock:
            f.write((json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8"))
            f.flush()

    first = f.readline()
    if not first:
        return
    hello = json.loads(first.decode("utf-8"))
    log.write(first.decode("utf-8")); log.flush()
    send({"type": "link", "protocol": 1, "server": "fake TowerTrainer"})
    send({"type": "welcome", "code": hello.get("code", "TOWER"), "id": my_id, "name": "EETN viktor", "roster": [], "host": True, "chat": []})
    send(AIRPORT)
    t0 = time.time()
    stop = False

    def pump():
        while not stop:
            try:
                send(state(time.time() - t0, claims, my_id, bays))
            except OSError:
                return
            time.sleep(0.25)

    threading.Thread(target=pump, daemon=True).start()
    try:
        while True:
            line = f.readline()
            if not line:
                break
            log.write(line.decode("utf-8", "replace")); log.flush()
            try:
                m = json.loads(line.decode("utf-8"))
            except ValueError:
                continue
            t = m.get("type")
            if t == "claim":
                claims[my_id] = str(m.get("pos", "")).upper()
                send({"type": "claims", "claims": claims, "roster": [{"id": my_id, "name": "X-Plane", "role": "guest", "pos": claims[my_id]}]})
            elif t == "ping":
                send({"type": "pong", "t": m.get("t")})
            elif t == "tx":
                send({"type": "radio", "m": {"from": "atc", "text": m.get("text", ""), "pos": m.get("pos", "TWR"), "by": "X-Plane"}})
                send({"type": "radio", "m": {"from": "pilot", "text": "Roger, " + str(m.get("text", ""))[:40], "pos": m.get("pos", "TWR"), "ac": {"id": 1, "cs": "FIN321"}}})
            elif t == "strip" and m.get("accept"):
                send({"type": "alert", "msg": "strip accepted"})
            elif t == "strip" and m.get("bay"):
                states = {a["id"]: a["state"] for a in state(0, claims, my_id, {})["ac"]}
                bays[int(m.get("id", 0))] = {"bay": str(m["bay"]), "state": states.get(int(m.get("id", 0)), "")}
    finally:
        stop = True
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8781)
    ap.add_argument("--log", default="fake_towertrainer_received.txt")
    ap.add_argument("--dump", help="write the airport line and one state line to this file, and exit")
    a = ap.parse_args()
    if a.dump:
        st = state(30.0, {"x1": "TWR"}, "x1", {})
        st["utc"] = 1760600000000           # a fixed clock, so the fixture does not change every time it is made
        for x in st["ac"]:
            if x["callingUntil"]:
                x["callingUntil"] = st["utc"] + 2500
        with open(a.dump, "w", encoding="utf-8", newline="\n") as out:
            out.write(json.dumps(AIRPORT, separators=(",", ":")) + "\n")
            out.write(json.dumps(st, separators=(",", ":")) + "\n")
        print(f"wrote {a.dump}")
        return
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", a.port))
    s.listen(4)
    print(f"fake TowerTrainer tower link on 127.0.0.1:{s.getsockname()[1]}, logging to {a.log}", flush=True)
    log = open(a.log, "a", encoding="utf-8")
    while True:
        conn, addr = s.accept()
        threading.Thread(target=serve, args=(conn, addr, log), daemon=True).start()


if __name__ == "__main__":
    main()
