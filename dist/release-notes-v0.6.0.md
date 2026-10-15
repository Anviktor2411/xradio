## XRadio v0.6.0 -- air traffic control

One zip for Windows, macOS (Intel + Apple Silicon) and Linux. Unzip into
`X-Plane 12/Resources/plugins/` so you get `plugins/XRadio/`.

**Everyone flying together must be on this version** -- the wire protocol is
now v6, it is checked at login, and a v0.5.x build is refused with the reason
on screen ("the server runs another XRadio version").

### New

- **ATC stations.** A controller program -- [TowerTrainer](https://github.com/Anviktor2411/TowerTrainer)
  is the first -- logs in with the `LF_ATC` flag: it is not an aircraft, so it is
  not drawn and not on anyone's TCAS, but it hears and speaks on its frequency,
  sees all traffic in range, receives every flight plan and may publish an ATIS.
- **ATC online list** in the XRadio window: callsign, position (Tower, Ground…)
  and frequency of every station in the flight. Click a station to tune COM1,
  click an ATIS to tune COM2. New stations are announced in the radio log.
- **Flight plans.** Settings → ATC: rules, departure, destination, alternate,
  cruise altitude, route, remarks, and a *File flight plan* button. The plan
  reaches every controller; their answer (acknowledged, squawk assigned,
  clearance) comes back into the window and the radio log, read out by the
  sim if you want.
- **ATIS as text.** Tune the ATIS frequency and the current information letter
  and text appear in the window, with a note whenever the letter changes.
- **Computer-flown traffic.** An ATC program drives a whole airport of aircraft
  through one session (`PT_AI_TRAFFIC`); they arrive as ordinary traffic,
  drawn as CSL models and on TCAS, tagged *AI* in the traffic list, with a cap
  of their own (Settings → ATC, nearest first) and the option to leave them out
  of the list. Their radio calls are relayed in their own callsigns.
- **Calls from ATC and private messages are read out** by the sim's voice
  (Settings → ATC, on by default), so nothing is missed with the window shut.
- `tools/fake_client.py` can now be a station (`--atc --atis … --ack-plans 4521`),
  an injector (`--inject 12`), file a plan (`--file-plan "EETN EFHK DCT"`) and
  print the ATC list, ATIS texts, flight-plan answers and the shared weather.

### Fixed

- The Python relay server on Windows lost other pilots' packets whenever one
  pilot's X-Plane had closed without logging out (`SIO_UDP_CONNRESET`); the
  built-in C++ server already handled it.
- The test executables build without sanitizers (`-DXRADIO_SANITIZE=OFF`) on a
  toolchain that has none, MinGW-w64 for one.

### Protocol v6

`PT_AI_TRAFFIC` (13), `PT_FLIGHTPLAN` (14), `PT_ATIS` (15), `PT_ATC_LIST` (16),
login flags `LF_ATC` and `LF_INJECTOR`. Everything older is unchanged; see
`docs/atc.md` for the packets and `plugin/src/protocol.h` for the structs.
`tools/check_sizes.py --compare` holds the C++ and Python definitions together,
`tools/test_server.py` and `tools/test_server_parity.py` cover the routing in
both servers.

**Full Changelog**: https://github.com/Anviktor2411/xradio/compare/v0.5.7...v0.6.0
