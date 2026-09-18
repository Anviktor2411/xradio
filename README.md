# XRadio

A multiplayer and radio plugin for X-Plane 12. Pilots see each other in the sky
as real aircraft (CSL models) and talk to each other by voice on the COM
frequencies they have tuned, push-to-talk, like the real thing.

Supported: **Windows, macOS (Intel + Apple Silicon), Linux**.

## Architecture

```
  X-Plane 12                                  your server
  ┌──────────────────────────┐                ┌────────────────────┐
  │  XRadio plugin (C++)     │  UDP :49100    │  server.py         │
  │  · reads datarefs        │ ─────────────► │  · sessions        │
  │  · sends its position    │ ◄───────────── │  · traffic at 5 Hz │
  │  · XPMP2 → CSL models    │                │  · frequency-based │
  │  · PTT → Opus voice      │                │    routing         │
  └──────────────────────────┘                └────────────────────┘
```

Voice is **[Opus](https://opus-codec.org/)** at 24 kbit/s — the codec behind
Discord and WhatsApp calls — captured and played through
**[miniaudio](https://miniaud.io/)**, which talks to WASAPI, CoreAudio, ALSA or
PulseAudio at run time so the plugin has no audio library dependencies.

Rendering other aircraft is handled by
**[XPMP2](https://github.com/TwinFan/XPMP2)** — the same library LiveTraffic and
xPilot use. It takes care of loading CSL models, the X-Plane instancing API,
the TCAS override and the map layer, and it already runs on all three
platforms.

The wire protocol is packed binary UDP, little-endian. It is defined in two
places and the two **must stay in sync**:

- `plugin/src/protocol.h` (C++)
- `server/protocol.py` (Python)

`tools/check_sizes.cpp` verifies that the struct sizes match.

## Running the server

```bash
cd server
python3 server.py --host 0.0.0.0 --port 49100 -v
```

No dependencies beyond Python 3.10+. Open **UDP** port 49100 in your firewall.
A systemd unit is provided in `server/xradio.service`.

## Tests

```bash
python3 tools/test_server.py          # server: protocol and routing
```

Starts a real server on a spare port and drives it over real UDP, covering the
wire format, login, traffic filtering, frequency and range routing, hostile
input, the length cap and session timeouts.

The plugin is tested too, without needing X-Plane. `tools/harness/` implements
just enough of the XPLM API for the real plugin code to run as an ordinary
executable, so CI can point it at a real server and check what its window
would display:

```bash
cmake -B build-tests -DXRADIO_BUILD_TESTS=ON -DXRADIO_USE_XPMP2=OFF \
      -DXPLANE_SDK=$PWD/tools/fake_sdk
cmake --build build-tests -j

./build-tests/voice_test
./build-tests/placement_test

python3 server/server.py --port 49400 &
python3 tools/fake_client.py --port 49400 --callsign PEER01 --lat 57.86 --lon 27.03 --talk --parrot &
./build-tests/plugin_harness 127.0.0.1 49400 CIRUN
```

The harness keys the PTT for two seconds; `--parrot` makes the peer send every
voice frame it hears straight back, so the run proves the whole loop —
microphone, encoder, server, decoder, mixer — end to end. It exits non-zero
if fewer than the expected frames make it round.

`tools/harness/voice_test.cpp` pushes a tone through the microphone path,
Opus, a simulated network and the playback mixer with no audio hardware, and
checks half-duplex muting, packet-loss concealment, sequence wrap-around and
garbage frames.

`tools/harness/placement_test.cpp` drives `XPluginStart` under several monitor
layouts — including a second monitor to the *left* of the main one, which makes
X-Plane's global desktop start at a negative x — and asserts both windows land
fully on screen.

`tools/harness/fuzz_server.py` is the same harness pointed at a deliberately
hostile server that answers with truncated, inconsistent and random packets.
Run under AddressSanitizer it proves the packet parser cannot be pushed out of
bounds. CI runs all of this on every push, plus a check that the C++ and
Python struct layouts still agree byte for byte.

## Testing without X-Plane

In two terminals:

```bash
python3 tools/fake_client.py --callsign ESNA12 --lat 57.85 --lon 27.02 --talk
python3 tools/fake_client.py --callsign ESNB34 --lat 57.88 --lon 27.05 --heading 250
```

Both should list each other on the `traffic:` line, and ESNB34 should receive
ESNA12's text message.

## Building the plugin

Requirements: git, CMake 3.16+, and a C++17 compiler
(Windows: Visual Studio 2022 C++ tools; macOS: Xcode command line tools;
Linux: gcc or clang).

**1. Fetch dependencies.** XPMP2 (which bundles a complete X-Plane SDK), Opus
and miniaudio, all into `lib/`:

```bash
./tools/fetch_deps.sh                                           # macOS / Linux
powershell -ExecutionPolicy Bypass -File tools\fetch_deps.ps1   # Windows
```

**2. Build:**

```bash
# macOS / Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Windows
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The result follows X-Plane's fat-plugin layout:

```
build/XRadio/
  win_x64/XRadio.xpl     # produced on Windows
  mac_x64/XRadio.xpl     # produced on macOS
  lin_x64/XRadio.xpl     # produced on Linux
  Resources/             # XPMP2 data files (related.txt, Doc8643.txt, ...)
```

**3. Install:** copy the `build/XRadio` folder to
`X-Plane 12/Resources/plugins/XRadio/`.

For a single package that works on all three platforms, build all three and put
`win_x64`, `mac_x64` and `lin_x64` in the same `XRadio/` folder. The GitHub
Actions workflow (`.github/workflows/build.yml`) does this for you: every push
builds all three and uploads the results as artifacts.

The macOS build is a universal binary (arm64 + x86_64).

The `.xpl` must be named after the plugin folder — `XRadio/win_x64/XRadio.xpl`,
not `win_x64/win.xpl`. That is the SDK 3.0 rule; with the wrong name X-Plane
skips the folder without writing anything to `Log.txt`, so it looks as though
the plugin does not exist.

### Building with parts switched off

```bash
cmake -B build -DXRADIO_USE_XPMP2=OFF -DXPLANE_SDK=<path to SDK>   # no 3D models
cmake -B build -DXRADIO_USE_VOICE=OFF                             # no voice
```

Without XPMP2 other pilots only appear in the window's list; without voice the
PTT still marks you as transmitting but nothing is sent.

## CSL models

XPMP2 needs CSL models in OBJ8 format. Without them everything still works, but
other aircraft are invisible (a warning appears in `Log.txt`).

Put model packages here:

```
X-Plane 12/Resources/plugins/XRadio/Resources/CSL/<package>/
```

Each package root needs an `xsb_aircraft.txt`. A free, open set is
**Bluebell** (`https://github.com/oktalist/BluebellCSL`), which covers most
common types. When no model matches, XPMP2 falls back to the `actype` from
`xradio.cfg`.

## Configuration

Easiest way: **Plugins → XRadio → Settings...** in the sim. Click a field,
type, Tab to move on, Enter to save. Saving writes the config file and
reconnects immediately — no restart, no text editor. Escape or Cancel backs
out without changing anything.

The same values live in `X-Plane 12/Output/preferences/xradio.cfg`, written on
first run:

```ini
host = your.server.address
port = 49100
callsign = ESNA12
actype = C172
```

`actype` is the ICAO type code that decides which CSL model other pilots see
you as. If you edit the file by hand while X-Plane is running, pick up the
changes with *Plugins → XRadio → Reconnect*.

If a window ever ends up somewhere you cannot reach it — dragged off-screen,
or the monitor layout changed while X-Plane was running — use
*Plugins → XRadio → Reset window position*.

**PTT key:** in X-Plane's *Keyboard* or *Joystick* settings, search for the
command `xradio/ptt` and bind it to a key or joystick button. Hold it to talk
on whichever COM the audio panel has selected for transmit; release it to
listen. Like a real radio it is half-duplex — you do not hear others while you
are keyed.

## Voice

The window's `Voice:` line tells you what is going on:

| shows | meaning |
|---|---|
| `OK  mic: <device>` | microphone and speakers opened; you are set |
| `no microphone (receive only)` | no input device found — you can hear but not talk |
| `no speakers` / `no audio backend` | voice is off; see `Log.txt` |
| `MIC [######....]` | live level while the PTT is held — if this stays at dots, X-Plane is not getting your microphone |
| `RX: SU-CBB` | who you are hearing right now |

Voice uses the system default microphone and output device. Windows: check
*Settings → System → Sound → Input* is the headset you mean. macOS: the first
key press may trigger the microphone permission prompt; grant it and press
again.

Each transmission is Opus at 24 kbit/s — about 3 KB/s per person talking.
A 300–3400 Hz band-pass and a faint carrier hiss give it the radio sound.

## What works today

- Login, automatic reconnect, session timeouts
- Own position sent at 5 Hz (location, attitude, speed, gear, flaps, lights)
- **Other aircraft actually visible** — CSL models, TCAS, map layer, animated
  gear/flaps/lights, callsign label
- Whoever is transmitting on a frequency you monitor gets a green `[TX]` label
- COM1/COM2 frequencies and audio panel read from the sim
- Text messages routed by frequency and VHF line-of-sight range, without
  needing the PTT held; the server rejects transmissions on a frequency you
  are not actually tuned to
- Dead reckoning between the 5 Hz position updates, so other aircraft move
  smoothly instead of stepping forward five times a second
- **Voice on the COM frequencies** — push-to-talk, Opus at 24 kbit/s, 20 ms
  frames, packet-loss concealment, half-duplex, radio band-pass; runs on its
  own network thread so audio never waits for a frame
- In-sim settings window for server, port, callsign and aircraft type, with
  validation and immediate reconnect
- Windows placed against the monitor X-Plane is actually using, so they do not
  spawn off-screen on multi-monitor setups
- Builds on Windows, macOS and Linux; CI checks all three

### Two ranges, on purpose

Traffic is sent within 80 nm, but the radio reaches as far as the VHF horizon
(`1.23 × (√h₁ + √h₂)` in feet and nautical miles — about 135 nm for two
aircraft at 3000 ft). So you can hear someone you cannot see, exactly as in
real life.

### Untrusted input

Both ends treat the wire as hostile. The server drops position reports with
NaN, infinite or out-of-range values instead of relaying them onward, and
strips control characters from callsigns. The plugin does the same on the way
in: implausible traffic entries are skipped rather than handed to the
renderer, text is clamped and scrubbed before it reaches the window, and
strings that arrive without a null terminator are handled as fixed-width.

### Security

The server has no authentication: anyone who knows the address can join under
any callsign. Packets after login must carry the session id the server issued,
which stops blind off-path spoofing, but this is a flying-with-friends server,
not a hardened public service. Run it on a port you are willing to expose, and
do not reuse it for anything sensitive.

## Roadmap

1. **Microphone and output device selection** in the settings window, plus a
   volume slider — right now it is the system default device.
2. **Radio effects** — signal fading towards the edge of range, a "blocked"
   squeal when two people transmit at once.
3. **Text chat input field** in the window (receive-only for now).
4. **Smarter smoothing** — dead reckoning is in, but interpolating between two
   buffered samples would handle turns better than extrapolating from one.

## License

MIT — see `LICENSE`. XPMP2 is MIT, Opus is BSD-3 and miniaudio is public
domain / MIT-0, so all are compatible.
