# XRadio

A multiplayer and radio plugin for X-Plane 12. Pilots see each other in the sky
as real aircraft (CSL models) and talk to each other on COM frequencies.

Supported: **Windows, macOS (Intel + Apple Silicon), Linux**.

## Architecture

```
  X-Plane 12                                  your server
  ┌──────────────────────────┐                ┌────────────────────┐
  │  XRadio plugin (C++)     │  UDP :49100    │  server.py         │
  │  · reads datarefs        │ ─────────────► │  · sessions        │
  │  · sends its position    │ ◄───────────── │  · traffic at 5 Hz │
  │  · XPMP2 → CSL models    │                │  · frequency-based │
  │  · PTT + voice (TODO)    │                │    routing         │
  └──────────────────────────┘                └────────────────────┘
```

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

**1. Fetch dependencies.** XPMP2 bundles a complete X-Plane SDK, so this is the
only thing you need to download:

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
  win_x64/win.xpl        # produced on Windows
  mac_x64/mac.xpl        # produced on macOS
  lin_x64/lin.xpl        # produced on Linux
  Resources/             # XPMP2 data files (related.txt, Doc8643.txt, ...)
```

**3. Install:** copy the `build/XRadio` folder to
`X-Plane 12/Resources/plugins/XRadio/`.

For a single package that works on all three platforms, build all three and put
`win_x64`, `mac_x64` and `lin_x64` in the same `XRadio/` folder. The GitHub
Actions workflow (`.github/workflows/build.yml`) does this for you: every push
builds all three and uploads the results as artifacts.

The macOS build is a universal binary (arm64 + x86_64).

### Building without 3D models

To test just the networking side quickly:

```bash
cmake -B build -DXRADIO_USE_XPMP2=OFF -DXPLANE_SDK=<path to SDK>
```

Other pilots then only appear in the plugin window's list.

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

On first run the plugin writes
`X-Plane 12/Output/preferences/xradio.cfg`:

```ini
host = your.server.address
port = 49100
callsign = ESNA12
actype = C172
```

`actype` is the ICAO type code that decides which CSL model other pilots see
you as. After editing, choose
*Plugins → XRadio → Reload config & reconnect* in X-Plane.

**PTT key:** in X-Plane's *Keyboard* or *Joystick* settings, search for the
command `xradio/ptt` and bind it to a key or joystick button.

## What works today

- Login, automatic reconnect, session timeouts
- Own position sent at 5 Hz (location, attitude, speed, gear, flaps, lights)
- **Other aircraft actually visible** — CSL models, TCAS, map layer, animated
  gear/flaps/lights, callsign label
- Whoever is transmitting on a frequency you monitor gets a green `[TX]` label
- COM1/COM2 frequencies and audio panel read from the sim
- Text messages routed by frequency and VHF line-of-sight range
- PTT command (currently only flags the TX state)
- Builds on Windows, macOS and Linux; CI checks all three

## Roadmap

1. **Voice** — microphone capture (miniaudio), Opus at 48 kHz mono in 20 ms
   frames, jitter buffer, mixing multiple speakers. Hooks in the code are
   marked `TODO(voice)`.
2. **Radio effects** — carrier noise, dropouts with distance, a "blocked"
   signal when two people transmit at once.
3. **Text chat input field** in the window (receive-only for now).
4. **Interpolation** — positions arrive at 5 Hz; extrapolating between packets
   would make other aircraft move more smoothly.

## License

MIT — see `LICENSE`. XPMP2 is MIT and Opus is BSD, so both are compatible.
