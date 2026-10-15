#!/usr/bin/env python3
"""A stand-in for TowerTrainer's tower link (server.py --tower-port), for the plugin's Tower-mode tests.

It speaks the same newline-separated JSON: after the plugin's hello it sends "link", "welcome", "airport"
and then a "state" four times a second (two aircraft at EETN, one of them an incoming handoff), answers
"claim" with "claims", "ping" with "pong", and writes every line it receives to --log so a test can check
what the plugin sent (claim, tx, strip accept, ops).

    python tools/fake_towertrainer.py --port 8781 --log received.txt
"""
import argparse
import json
import socket
import threading
import time

AIRPORT = {"type": "airport", "protocol": 1, "icao": "EETN", "name": "Lennart Meri Tallinn", "lat": 59.413333, "lon": 24.8325, "elev_ft": 131,
           "tower": [600.0, -250.0],
           "runways": [{"name": "08/26", "width": 45, "ends": [{"name": "08", "x": -1515.9, "y": 0.08, "displaced": 248}, {"name": "26", "x": 1965.6, "y": -17.8, "displaced": 0}]}],
           "holdLines": [{"id": 0, "x": 300.0, "y": 120.0}], "stands": [{"name": "K1", "x": 981.6, "y": -325.0, "hdg": 180}]}


def state(t, claims, my_id):
    fin = {"id": 1, "cs": "FIN321", "tel": "Finnair", "num": "three two one", "type": "A320", "wtc": "M", "kind": "dep", "state": "holding", "pos": [1900.0 - t * 0.0, -40.0], "alt": 131, "hdg": 260,
           "spd": 0, "onGround": True, "freq": "TWR", "squawk": "4521", "other": ["EFHK", "Helsinki"], "handoff": None, "stand": None}
    sas = {"id": 2, "cs": "SAS123", "tel": "Scandinavian", "num": "one two three", "type": "B738", "wtc": "M", "kind": "dep", "state": "taxi", "pos": [400.0 + t * 5.0, 200.0], "alt": 131, "hdg": 90,
           "spd": 8.0, "onGround": True, "freq": "TWR", "squawk": "4522", "other": ["ESSA", "Stockholm"], "handoff": {"from": "GND", "to": "TWR", "accepted": False, "age": int(t)}, "stand": None}
    arr = {"id": 3, "cs": "DLH5KM", "tel": "Lufthansa", "num": "five kilo mike", "type": "A321", "wtc": "M", "kind": "arr", "state": "final", "pos": [-9000.0 + t * 70.0, 300.0], "alt": 2500 - t * 15, "hdg": 80,
           "spd": 72.0, "onGround": False, "freq": "TWR", "squawk": "4530", "other": ["EDDF", "Frankfurt"], "handoff": None, "stand": None}
    pos = {k: {"key": k, "cs": f"EETN_{k}" if k != "CTR" else "EE_CTR", "name": n, "freq": f, "mhz": float(f), "unit": u} for k, n, f, u in
           [("DEL", "Tallinn Delivery", "121.805", "Delivery"), ("GND", "Tallinn Ground", "131.905", "Ground"), ("TWR", "Tallinn Tower", "135.905", "Tower"), ("APP", "Tallinn Radar", "127.905", "Approach"), ("CTR", "Tallinn Control", "132.805", "Control")]}
    return {"type": "state", "icao": "EETN", "t": f"{int(t) // 60}:{int(t) % 60:02d}", "ac": [fin, sas, arr], "split": True, "positions": pos,
            "staffed": ["TWR"] + (["TWR"] if claims.get(my_id) == "TWR" else []), "selectedId": None, "rwy": "26", "arrEnd": "26",
            "atis": "<b>A</b> <span class=\"raw\">METAR EETN 150620Z 20006KT 9999 OVC007 09/07 Q1022</span> RWY 26 wind 200/6",
            "closed": [], "lvp": False, "seq": "", "wx": "", "del": "", "seqOn": False, "chat": [], "metar": "METAR EETN 150620Z 20006KT 9999 OVC007 09/07 Q1022",
            "claims": claims, "roster": [{"id": my_id, "name": "X-Plane", "role": "guest", "pos": claims.get(my_id, "")}], "coords": [], "remoteStaff": {claims[my_id]: "X-Plane"} if claims.get(my_id) else {},
            "hostNick": "viktor", "logs": {"TWR": [{"cls": "sys", "html": "EETN · runway 26"}, {"cls": "pilot dep", "html": "<b>FIN321</b> Tallinn Tower, Finnair three two one, holding point runway 26, ready for departure"}]}}


def serve(conn, addr, log):
    f = conn.makefile("rwb")
    claims = {}
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
                send(state(time.time() - t0, claims, my_id))
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
    a = ap.parse_args()
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
