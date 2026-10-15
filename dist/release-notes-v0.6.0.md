## XRadio v0.6.0 -- air traffic control

One zip for Windows, macOS (Intel + Apple Silicon) and Linux. Unzip into
`X-Plane 12/Resources/plugins/` so you get `plugins/XRadio/`.

**Everyone flying together must be on this version** -- the wire protocol is
now v6, it is checked at login, and a v0.5.x build is refused with the reason
on screen ("the server runs another XRadio version").

### New

- **A new window.** One window with a sidebar, drawn with Dear ImGui: a COM1/COM2 radio stack with
  station names, RX/TX lights and signal strength; controllers online (click to tune, AI badges, ATIS);
  the pilots with distance, frequency and who is talking; chat-style messages with ATC and private ones
  highlighted, unread counts and quick replies; the flight plan with SimBrief import and the plan next to
  the clearance you received; hosting and connecting with the address to share, router status and
  connection quality; grouped settings with a mic test and key capture; English or Estonian; dark and
  light themes; a compact cockpit overlay with toasts; a first-run wizard. Pops out and works in VR.
  Screenshots in `docs/ui.md`. The old text window stays as a build option.
- **SimBrief.** Flight plan → *Import from SimBrief* fetches your latest OFP through SimBrief's
  documented fetch API (username or Pilot ID in Settings → SimBrief; only when you press the button)
  and fills the plan -- route with SID and STAR, cruise level, TAS, EOBT, equipment, alternate -- for you
  to edit and *File*. The OFP's runways, fuel, weights, times and PDF link stay on the tab, *Write
  X-Plane .fms* saves the route for the FMS (never overwriting without a second press), and the
  clearance ATC answers with is shown next to the plan with differences marked. HTTP 400 (unknown user
  or no flight planned yet) is explained in plain words; *Plan on SimBrief* opens SimBrief's own page.
  The first-run wizard asks for the SimBrief username as its optional last step.
- **Tower mode.** Plugins → XRadio → *Tower mode* (`xradio/tower/toggle`): the camera moves to the
  tower cab of the airport TowerTrainer is running (the `apt.dat` viewpoint, else the OpenStreetMap
  tower), your aircraft is frozen and restored exactly on leaving, and five windows show the trainer's
  radar as text, the strips (ACCEPT incoming handoffs), the Delivery list, your frequency with a command
  line, and ATIS / operations (LVP, runway, stop bars, light gun). Look around by dragging, binoculars
  with the wheel, follow an aircraft, a view from the apron; all bindable commands under `xradio/tower/`.
  Works standalone against TowerTrainer's `server.py` (its traffic drawn as CSL models) or together with
  an XRadio flight. Settings → Tower. See `docs/tower-mode.md`.

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
