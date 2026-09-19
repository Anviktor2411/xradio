# XRadio

A multiplayer and radio plugin for X-Plane 12. Pilots see each other in the sky
as real aircraft (CSL models) and talk to each other by voice on the COM
frequencies they have tuned, push-to-talk, like the real thing.

Supported: **Windows, macOS (Intel + Apple Silicon), Linux**.

## Flying together

Nobody has to set a server up.

One of you opens **Plugins → XRadio → Settings → Hosting**, ticks *Host a
flight here* and saves. The plugin starts the relay server itself, connects
to it, asks the router to open the port, and then shows the address to pass
on:

```
  Running on port 49100  ·  1 connected
  Friends type:  81.90.144.12:49100
  Router opened the port (Livebox)
  Here now: ESNA12
```

Everyone else puts that address into **Settings → Connection** and saves.
That is the whole procedure — no VPS, no Python, no terminal, and nothing to
configure on anyone's router.

If the router will not open the port, the window says so instead of leaving
you guessing, and people on the same network as the host can still join using
the host's local address. A dedicated server is still the better answer for a
group that wants to fly without waiting for one particular person to be
online — see [Running a dedicated server](#running-a-dedicated-server).

## Architecture

```
  the host's X-Plane                          everyone else
  ┌──────────────────────────┐                ┌──────────────────────────┐
  │  XRadio plugin (C++)     │                │  XRadio plugin           │
  │  · reads datarefs        │                │                          │
  │  · sends its position    │                │                          │
  │  · XPMP2 → CSL models    │                │                          │
  │  · PTT → Opus voice      │                │                          │
  │  ─────────────────────   │   UDP :49100   │                          │
  │  relay server (thread)   │ ◄───────────── │                          │
  │  · sessions              │ ─────────────► │                          │
  │  · traffic at 10 Hz      │                │                          │
  │  · frequency routing     │                │                          │
  └──────────────────────────┘                └──────────────────────────┘
```

The relay server exists twice, on purpose: `plugin/src/server.cpp` is the one
built into the plugin, and `server/server.py` is the same thing for a machine
that should stay up without X-Plane running. They speak one protocol, so a
difference between them is a bug — `tools/test_server_parity.py` drives the
same scenarios at both over real UDP and fails if the transcripts differ.

Voice is **[Opus](https://opus-codec.org/)** at 24 kbit/s — the codec behind
Discord and WhatsApp calls — captured and played through
**[miniaudio](https://miniaud.io/)**, which talks to WASAPI, CoreAudio, ALSA or
PulseAudio at run time so the plugin has no audio library dependencies.

Rendering other aircraft is handled by
**[XPMP2](https://github.com/TwinFan/XPMP2)** — the same library LiveTraffic and
xPilot use. It takes care of loading CSL models, the X-Plane instancing API,
the TCAS override and the map layer, and it already runs on all three
platforms.

Other aircraft are **interpolated in the sender's own timeline** rather than
drawn as reports arrive — see `plugin/src/smoothing.h`. Each position report
carries the sender's timestamp, and receivers render a fixed 350 ms behind the
newest one. Network and relay timing then cannot affect the motion at all.

The wire protocol is packed binary UDP, little-endian. It is defined in two
places and the two **must stay in sync**:

- `plugin/src/protocol.h` (C++)
- `server/protocol.py` (Python)

`tools/check_sizes.cpp` verifies that the struct sizes match.

## Running a dedicated server

Only worth it if you want a server that is up whether or not any particular
person is flying. For an evening with friends, hosting from inside the sim is
simpler and does the same job.

```bash
cd server
python3 server.py --host 0.0.0.0 --port 49100 -v
```

No dependencies beyond Python 3.10+. Open **UDP** port 49100 in your firewall.
A systemd unit is provided in `server/xradio.service`.

The release package also carries the same server as a compiled binary, for a
machine without Python:

```bash
./server/lin_x64/xradio_server 49100        # or win_x64\ , mac_x64/
```

## Tests

```bash
bash tools/check_portability.sh       # instant: MSVC-only pitfalls
bash tools/check_windows_build.sh     # ~20s: compile everything for Windows
python3 tools/test_server.py          # server: protocol and routing
```

`check_portability.sh` catches the mistakes that pass on Linux and macOS and
fail on Windows ten minutes later — `M_PI` (POSIX, not standard C++, and MSVC
only defines it when `_USE_MATH_DEFINES` precedes `<cmath>`), unguarded POSIX
headers, variable-length arrays, `std::min` without `<algorithm>`, and a
Win32 symbol used without the header that declares it. Use `xr::kPi` from
`plugin/src/mathconst.h` rather than `M_PI`.

`check_windows_build.sh` goes further and actually compiles every source file
for Windows with MinGW, from Linux, in about twenty seconds. Code inside
`#ifdef _WIN32` is invisible to the Linux and macOS compilers, so a missing
header there passes two of the three CI jobs and fails the third ten minutes
in — `SIO_UDP_CONNRESET` is declared in `<mstcpip.h>`, not `<winsock2.h>`,
and that is exactly how it was found. MinGW is not MSVC and will not catch
everything, but it compiles the Windows branches against real Windows
headers, which is where that whole class of mistake lives.

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
./build-tests/settings_test
./build-tests/hosting_test
python3 tools/test_server_parity.py

./build-tests/xradio_server 49700 &
python3 tools/harness/fuzz_client.py --port 49700

python3 server/server.py --port 49400 &
python3 tools/fake_client.py --port 49400 --callsign PEER01 --lat 57.86 --lon 27.03 --talk --parrot &
./build-tests/plugin_harness 127.0.0.1 49400 CIRUN
```

The harness keys the PTT for two seconds; `--parrot` makes the peer send every
voice frame it hears straight back, so the run proves the whole loop —
microphone, encoder, server, decoder, mixer — end to end. It exits non-zero
if fewer than the expected frames make it round.

`tools/harness/smoothing_test.cpp` flies a simulated aircraft through a turn,
pushes its reports through a relay that samples on its own clock (duplicating
and skipping some) and a jittery network, renders at 60 fps, and asserts the
motion never runs backwards or jumps. It runs the previous dead-reckoning
approach through the same scenario first, to prove the test catches the bug.

`tools/harness/voice_test.cpp` pushes a tone through the microphone path,
Opus, a simulated network and the playback mixer with no audio hardware, and
checks half-duplex muting, packet-loss concealment, sequence wrap-around and
garbage frames.

`tools/harness/settings_test.cpp` covers the settings window the way a pilot
uses it: it reads back the strings the window draws, clicks the rows those
strings were actually drawn on, switches tabs, drags sliders, flips toggles,
types into fields, and checks what lands in `xradio.cfg`. It also asserts the
things that are easy to get wrong and impossible to see — that a burst of
keystrokes arriving in one frame all land in the field that was focused when
each was typed, and that Enter saves the character typed just before it.

`tools/harness/hosting_test.cpp` is the hosting feature end to end: it ticks
*Host a flight here* in the settings window, waits for the plugin's own client
to connect to its own server, then opens a real UDP socket and joins as a
second pilot. It checks that each sees the other's aircraft, that text
reaches both ways through the hosted server, that the Hosting tab reports the
truth, and that unticking the box really does stop the server and refuse new
joins.

`tools/test_server_parity.py` is a differential test between the two server
implementations. It starts a fresh Python server and a fresh C++ server for
each of eight scenarios, drives identical traffic at them over real UDP, and
compares what the clients receive. Every position-validation rule is probed
with its own timestamped report, so dropping one rule from one server shows
up as a state the watching client should never have seen.

`tools/harness/placement_test.cpp` drives `XPluginStart` under several monitor
layouts — including a second monitor to the *left* of the main one, which makes
X-Plane's global desktop start at a negative x — and asserts both windows land
fully on screen.

`tools/harness/fuzz_client.py` is the mirror image, and it matters more now
that hosting puts the server on a pilot's home connection: it throws ~11,000
malformed, truncated, length-lying and outright random datagrams at the
server while a real session runs alongside, then checks the server is still
answering and still accepting new pilots. Run against the ASan build, one
out-of-bounds read stops it with a stack trace.

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

## Protocol versions

The header carries a version and the server rejects anything that does not
match, so **all pilots and the server must run the same build**. v2 added the
position timestamp, ground track and vertical speed that the smoothing needs;
a v1 client gets no error beyond being ignored.

## Configuration

Easiest way: **Plugins → XRadio → Settings...** in the sim. The window has
three tabs; click a field and type, click a toggle to flip it, click anywhere
on a slider's bar to set it, click the `< >` arrows to step through the audio
devices. Tab moves to the next field, Enter saves, Escape or Cancel backs out
without changing anything. Saving writes the config file and applies
everything immediately — no restart, no text editor; only a change to the
server, port, callsign or aircraft type causes a reconnect.

**Connection**

| setting | what it does |
|---|---|
| Server host / Port | where to connect |
| Callsign | how you appear to everyone else; upper-cased on save |
| Aircraft type | ICAO type code that picks the CSL model others see you as |
| Connect on startup | off means you connect by hand from the menu |
| Report rate | position reports per second, 1–10. Lower it on a weak uplink |
| Smoothing delay | how far behind the newest report other aircraft are drawn, 100–1000 ms. Higher rides out worse jitter at the cost of lag |

**Audio**

| setting | what it does |
|---|---|
| Microphone / Output | pick a device, or leave empty for the system default |
| Volume | incoming radio volume |
| Hear own voice | sidetone: hear yourself while keyed, like a real headset |
| Radio noise | scales every bit of noise the radio makes: the faint floor under a strong signal, the hiss that grows towards the horizon, the squelch bursts. 0 for none |
| Radio sound | the whole VHF radio character — limiter, overdrive, squelch, 300–2700 Hz filter, distance fading, the blocked squeal. Off = clean audio |

The audio tab also shows a live mic level meter and the voice status line, so
you can hold the PTT and confirm the right microphone is being heard before
you go looking for someone to talk to.

**Hosting**

| setting | what it does |
|---|---|
| Host a flight here | run the relay server inside the plugin. Your own aircraft connects to it, so the Connection tab is ignored while this is on |
| Port to host on | the UDP port friends connect to. 49100 unless something else is using it |
| Ask the router to open it | UPnP: opens the port automatically so nobody touches a router config page. Off if you have forwarded the port yourself, or if your router's UPnP is switched off |

**Traffic**

| setting | what it does |
|---|---|
| Draw other aircraft | off removes the CSL models; the radio keeps working |
| Callsign labels | the floating name tags |
| Label range | how far away labels stay readable, 1–100 nm |
| Traffic range | how far away aircraft are drawn, 5–200 nm |

The same values live in `X-Plane 12/Output/preferences/xradio.cfg`, written on
first run:

```ini
host = your.server.address
port = 49100
callsign = ESNA12
actype = C172
autoconnect = yes
reporthz = 5
smoothms = 350

mic =
speakers =
volume = 0.8
sidetone = no
hiss = 0.35
radiofilter = yes

showtraffic = yes
showlabels = yes
labeldist = 20
range = 80

hosting = no
hostport = 49100
hostupnp = yes
```

Out-of-range numbers are clamped to the limits above rather than rejected, and
unknown keys are ignored, so a hand-edited file cannot stop the plugin
loading. If you edit it while X-Plane is running, pick up the changes with
*Plugins → XRadio → Reconnect*.

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

Pick the microphone and output device on the settings window's *Audio* tab,
or leave them empty for the system default. Windows: check
*Settings → System → Sound → Input* is the headset you mean. macOS: the first
key press may trigger the microphone permission prompt; grant it and press
again.

Each transmission is Opus at 24 kbit/s — about 3 KB/s per person talking.

### The radio sound

A band-pass on its own sounds like a telephone. What makes a COM radio sound
like one is the rest of the chain, so all of it is modelled on the receiving
side, in the order the real signal goes through it (`plugin/src/voice.cpp`,
`struct Radio`):

| stage | what it does |
|---|---|
| transmitter | a modulation limiter squashes every syllable to the same level, then a soft overdrive adds the crunch that consonants get when a voice over-modulates |
| channel | noise that rises as the other aircraft nears the VHF horizon; near it the audio breaks up, whole 20 ms frames going missing; two people keying at once produce the beating heterodyne squeal |
| receiver | the squelch opens with a click and a burst of noise, closes with a longer burst and a click once the carrier drops; a 4th-order 300–2700 Hz audio filter; an output stage that cannot be driven past two thirds of full scale, whatever comes in |

Distance comes from the same VHF horizon the server uses to route
transmissions (`1.23 × (√h₁ + √h₂)` nm): a signal is clean to about half of
it, then degrades to nothing at the horizon, where the server stops relaying
it anyway. Someone the traffic list does not know about counts as strong.

To hear it without X-Plane, `tools/harness/radio_demo.cpp` renders a WAV
through the real pipeline — microphone path, Opus, the radio — and writes
four versions: clean, a strong signal, one from the horizon, and two people
transmitting at once:

```bash
./build-tests/radio_demo call.wav out      # out_clean.wav, out_strong.wav, ...
```

`tools/harness/voice_test.cpp` pins each of these down: the filter's cut-off,
the limiter bringing 24 dB of input spread to under 8 dB, the squelch
bursts and the silence after them, the noise rising with distance, the
dropouts, the squeal, and the output ceiling.

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
- **Smooth motion** — positions are timestamped at the sender and interpolated
  350 ms behind, so other aircraft move continuously regardless of network
  jitter or relay timing; extrapolation covers a short dropout, then it holds
- **Voice on the COM frequencies** — push-to-talk, Opus at 24 kbit/s, 20 ms
  frames, packet-loss concealment, half-duplex, radio band-pass; runs on its
  own network thread so audio never waits for a frame
- **Hosting from inside the sim** — one pilot ticks a box and the plugin runs
  the relay server itself, opens the router port over UPnP and shows the
  address to share; no VPS, Python or terminal for anyone
- **Tabbed in-sim settings window** — connection, audio (device pickers,
  volume, sidetone, hiss, radio filter, live mic meter), traffic (models,
  labels, ranges) and hosting — with validation and changes applied without a
  restart
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

Hosting opens a UDP port on your machine to the internet, and the relay
server behind it is the same code either way -- it validates every field of
every packet before relaying it, and the fuzzing below is run against it. It
is still a port, though: host when you want to fly with people, and untick
the box when you are done, which also asks the router to close it again.

The server has no authentication: anyone who knows the address can join under
any callsign. Packets after login must carry the session id the server issued,
which stops blind off-path spoofing, but this is a flying-with-friends server,
not a hardened public service. Run it on a port you are willing to expose, and
do not reuse it for anything sensitive.

## Roadmap

1. **Finding each other without swapping addresses** — a small list of public
   servers, or a code you can read out over the phone instead of an IP.
2. **Radio effects** — signal fading towards the edge of range, a "blocked"
   squeal when two people transmit at once.
3. **Text chat input field** in the window (receive-only for now).
4. **Key binding from the settings window** — the PTT is bound through
   X-Plane's own keyboard settings for now.
5. **Bandwidth** — traffic is relayed at 10 Hz to every client in range;
   scaling past a couple of dozen pilots wants per-client rate limiting by
   distance.

## License

MIT — see `LICENSE`. XPMP2 is MIT, Opus is BSD-3 and miniaudio is public
domain / MIT-0, so all are compatible.
