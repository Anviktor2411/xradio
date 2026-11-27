## XRadio v0.7.0 -- a shared cockpit, and more of the radio

One zip for Windows, macOS (Intel + Apple Silicon) and Linux. Unzip into
`X-Plane 12/Resources/plugins/` so you get `plugins/XRadio/`.

The wire protocol is unchanged (v6) and everything new travels in packet types
an older build drops, so a v0.7.0 plugin flies with v0.6.x pilots and servers.
The new features need v0.7.0 at both ends: a shared cockpit needs a v0.7.0
server and v0.7.0 on everybody aboard; kick, mute and the pilot limit need a
v0.7.0 server.

### New

- **Shared cockpit and intercom.** A pilot opens their cockpit (Settings →
  Crew: *my cockpit takes a crew*, optionally with a crew code); a copilot or
  an observer joins it from their own X-Plane (*I join a cockpit*, the
  pilot's callsign). Everyone aboard gets a private **intercom** -- its own
  push-to-talk (`xradio/intercom`, a key, or the button), its own volume, no
  range, no squelch, heard while you transmit on the radio. The crew hear
  what the **aircraft's** radios hear, from where it is; a copilot transmits
  on them *as the aircraft* (its callsign, its position, its [TX] flag; one
  transmitter, so the first to key holds it); the crew's own radio panels
  follow the aircraft's, and a knob a copilot turns retunes the aircraft.
  Crew are not drawn as aircraft of their own or put on anybody's TCAS.
  `docs/crew.md`.
- **More of the radio, each a switch.** A click in your own headset as you key
  and unkey; the squelch's burst and click on or off; static crashes on a weak
  signal, more towards the horizon and none on a strong one.
- **A microphone test that sends nothing.** Settings → Audio → *Mic test*:
  hold, speak, let go, and you hear yourself back through the codec and the
  whole radio chain -- as the others will. (The old button keyed the
  transmitter, so the whole frequency heard your test.)
- **Who is coming in.** Under "is transmitting" on the Radio tab: every
  station you hear with its frequency, signal bars and readability in words,
  distance, squawk and transponder mode.
- **Quick-tune.** Eight slots (`TWR=118.100;GND=121.800;...`), a row of buttons
  on the Radio tab (click COM1, right-click COM2, Ctrl+click to store) and the
  commands `xradio/preset_1` … `xradio/preset_8`. As a copilot they retune the
  aircraft.
- **Traffic scope.** A small TCAS-style window of the traffic around you
  (`xradio/traffic_window`, Plugins → XRadio, Settings → Display): range rings,
  relative altitudes in hundreds of feet, climb and descent arrows, proximate
  traffic filled amber, heading up or north up, 2 to 80 nm. Traffic on X-Plane's
  own map and on TCAS are now switches (Settings → Traffic).
- **Hosting controls.** While hosting, the Host / Connect tab lists everybody
  with **Mute**, **Kick** and **Kick 30 min**; *Most pilots* caps the flight
  (the next one is told it is full); *Findable on this network* answers the
  LAN search or not. A dedicated server gets the same with `--admin-token`,
  `--max-pilots`, `--hidden`, `--name` and the new `server/admin.py`
  (`docs/server.md`).
- **Find flights on this network.** One button on Host / Connect asks the local
  network who is hosting, with a *Join* button for each flight found.
- **The connection's health.** Round trip, jitter, packets lost and voice lost,
  on Host / Connect, in the text window and -- when it is bad enough to matter
  -- on the overlay and the traffic scope.

### Fixed

- A pilot heard from beyond traffic range showed up as a number instead of
  their callsign.
- The text window's settings (the build without the new window): it opens
  tall enough for its longest tab, a row cut off under the buttons can no
  longer take the click meant for *Save & apply* or *Cancel*, and a key
  pressed goes to the key row that was clicked. In both windows one key does
  one thing: given to the PTT, it is taken off the intercom.

### Docs

- `docs/crew.md` -- the shared cockpit; `docs/tailscale.md` -- flying together
  over Tailscale, no router setup; `docs/server.md` -- kick, mute, the pilot
  limit and the LAN search for whoever runs a server.

### Under the hood

- New packets `PT_CREW` (19), `PT_ADMIN` (20), `PT_DISCOVER` (21); the intercom
  is `PT_VOICE` / `PT_TEXT` on the sentinel frequency `kIntercomKhz` (1); a pong
  now echoes up to 16 bytes of its ping (an empty ping still gets an empty pong);
  `RJ_KICKED` and `RJ_FULL` say why a login was refused.
- Both servers implement all of it identically: `tools/test_server_parity.py`
  has six new scenarios (27 in all) compared byte for byte, `tools/test_server.py`
  covers it in Python, the client fuzzer throws the new packets at the C++ server
  under ASan/UBSan.
- New tests: `cockpit_test` (the crew panel logic, quick-tune, the round-trip
  meter), `crew_harness` (the real plugin as copilot and as pilot against fake
  pilots), and new sections in `voice_test` (the intercom path, the microphone
  test, the PTT click, the squelch tail, static crashes) and `ui_logic_test`
  (the scope's geometry, relative altitudes, the telemetry line).
- The version lives in `plugin/src/version.h` (and `server/protocol.py`'s copy,
  kept equal by `tools/release.py` and checked by `tools/check_sizes.py`).

### Trade-offs and what is not in it

- **No flight-model sync.** The shared cockpit shares the radios and the
  intercom, not the aircraft: for two pilots flying one aeroplane, use a
  shared-cockpit add-on for the aircraft and XRadio for the radios.
- **One transmitter per aircraft.** Two crew keying at once: the first holds
  it. A real audio panel would mix both microphones into the transmitter; the
  server cannot mix Opus, and two encoders interleaved into one stream is
  noise.
- **Kick bans an address**, so pilots behind one home router are kept out
  together. **The admin token and the crew code travel in the clear**, like the
  flight password: door locks, not cryptography.
- **The LAN search** sees what a broadcast reaches -- the local network, not
  the internet or (usually) a VPN.
- Talk flags in `CREW_STATE` are reserved: who is talking is known from the
  voice itself.

**Full Changelog**: https://github.com/Anviktor2411/xradio/compare/v0.6.1...v0.7.0
