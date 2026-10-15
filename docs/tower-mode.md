# Control from the tower in X-Plane (Tower mode)

Tower mode puts you in the tower cab of the airport TowerTrainer is running, inside X-Plane, with the
trainer's traffic in the sky and on the taxiways. TowerTrainer stays the authority: every instruction you
give goes to its simulation, and what you see in the windows is what its radar shows. It works with
XRadio connected to a flight (the AI traffic then comes through XRadio's injector, see `docs/atc.md`) and
just as well standalone against TowerTrainer's `server.py` alone.

## Set up

1. Start TowerTrainer (`start.bat` / `python server.py`). It opens a *tower link* on `127.0.0.1:8781`
   (`--tower-port 0` switches it off; `--host 0.0.0.0` lets another PC's X-Plane connect).
2. In the trainer: Settings → XRadio → tick **X-Plane tower link (session code TOWER)**. The trainer's
   window now hosts a multiplayer session with the code `TOWER` (you can also host one by hand under
   Multiplayer; then type its code in the plugin).
3. In X-Plane: XRadio → Settings → **Tower** tab: TowerTrainer's address and port, the session code, the
   position you work (TWR, GND, DEL, APP, CTR), your nickname, whether your own aircraft is frozen while
   you are in the tower, whether the trainer's visibility / wind / time should be set in X-Plane, the cab
   height (0 = from the scenery), and whether the trainer's traffic is drawn as CSL models when XRadio is
   not connected to a flight.
4. Plugins → XRadio → **Tower mode** (or bind the command `xradio/tower/toggle`).

## In the tower

* The camera goes to the cab: the tower viewpoint from the airport's `apt.dat` (row 14, custom scenery
  first), else the tower OpenStreetMap knows, else the apron centre, 30 m up. Drag in the radar window or
  use the commands `xradio/tower/look_left|right|up|down` to look around, the mouse wheel or
  `xradio/tower/zoom_in|out` for binoculars, **Follow** (or `xradio/tower/next_target`) to track an
  aircraft, `xradio/tower/ground_view` for a look from the apron, `xradio/tower/tower_view` to get back
  after X-Plane took the view (any view change does). In VR the cab is where your head is.
* Your own aircraft is frozen where it stood (`override_planepath`) and put back exactly — position,
  attitude, velocities — when you leave.
* Five windows (pop-out and VR capable like XRadio's others; `xradio/tower/windows` hides and shows them):
  **radar** — a character picture of the airport (runways `=`, holding points `.`, the tower `T`, aircraft
  by callsign; Range toggles 3 km / 15 nm); **strips** — incoming handoffs with ACCEPT, departures,
  arrivals; **Delivery** — clearance requests; **frequency** — the log of your position with a command
  line (Enter sends; the selected aircraft's callsign is filled in; `TX *` while you key XRadio's PTT);
  **ATIS / operations** — ATIS and METAR, LVP on/off, next runway, stop bars, light-gun signals for the
  selected aircraft.
* What you type is exactly what you would type in TowerTrainer: `FIN321 runway 26 cleared for takeoff`,
  `SAS123 taxi to holding point 26 via A B`, `DLH5KM light green steady`. The pilots answer in the
  trainer (and over XRadio's voice when the AI voices are on there).
* Speaking: key XRadio's push-to-talk as usual. The voice goes to the pilots on XRadio; the instruction
  itself still has to reach the trainer — type it, or keep the trainer's own window open with its
  speech recognition (Ctrl+Space), which hears the same microphone.

## How it talks to TowerTrainer

`towerlink.py` in TowerTrainer accepts a TCP connection and one JSON object per line. The first line is
`{"type":"hello","code":"TOWER","name":"X-Plane","pos":"TWR","version":"0.6.0"}`; after that the plugin is
an ordinary remote controller of the trainer's multiplayer session: it receives the same `state`
snapshots the trainer's pop-out windows get (plus an `airport` line with the datum, runways, holding
points and stands so metres become latitude and longitude), and sends `claim`, `tx`, `strip`, `ops`
(`lvp`, `runway`, `stopbars`) and `ping`. Nothing else travels; no audio, no files.

## Tests

`tools/harness/tower_test.cpp` (built with `-DXRADIO_BUILD_TESTS=ON`) checks the JSON reader, the
`apt.dat` viewpoint parser on `tools/fixtures/apt_tower.dat` (custom scenery first), the METAR parser,
and — with `tools/fake_towertrainer.py` running and `XR_TOWER_PORT` set — the whole round trip: entering
takes the camera to the trainer's tower, freezes the user's aircraft, the windows show the traffic, a click
on ACCEPT and a typed instruction reach the trainer, leaving restores the aircraft exactly. TowerTrainer's
side has `tests/towerlink_test.py`.
