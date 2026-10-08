# Hacking on XRadio

A map for anyone who wants to read, change or extend the code. The README
says what XRadio does and how to build it; this says how it is put together,
where things live, and the few rules that keep it working on three
platforms and two sims.

## The shape of it

```
plugin/src/      the X-Plane plugin, C++17
server/          the dedicated relay server, Python 3.10+, no dependencies
tools/           tests, the release script, fake routers and fake clients
tools/harness/   a fake X-Plane just big enough to run the real plugin as a program
docs/            this file, the server guide, the VPN guide
.github/         the build that runs every test and packages all three platforms
```

### plugin/src, file by file

| file | what it is |
|---|---|
| `main.cpp` | the plugin itself: X-Plane entry points, datarefs, the flight loop, the network thread, the main window, the settings window, the notices, hosting. Start here. |
| `protocol.h` | the wire format. Every struct that crosses the network, byte-packed. Has a twin in `server/protocol.py`. |
| `server.cpp/.h` | the relay server built into the plugin. `Core` is the logic with no socket; the rest is the socket and the thread. Has a twin in `server/server.py`. |
| `net.cpp/.h` | UDP sockets for Windows, macOS and Linux. The only file that knows Winsock from BSD sockets. |
| `voice.cpp/.h` | microphone to Opus to the network, and back to the speakers, with the sound of a VHF radio modelled on the way in. |
| `xpmp_bridge.cpp/.h` | other pilots as aircraft in the sim, through XPMP2. |
| `smoothing.h` | turns five position reports a second into continuous motion. Pure maths, no X-Plane. |
| `terrain.h` | where the ground is, asked of the sim, so models stand on it. |
| `weather.cpp/.h` | the host's sky, read out of X-Plane 12's region weather and written back into everyone else's. |
| `upnp.cpp/.h`, `natpmp.cpp/.h` | asking the router to open the hosting port, two protocols, on a worker thread. |
| `update.cpp/.h` | the once-per-session "a newer version exists" check. |
| `clipboard.cpp/.h` | the Copy buttons. |
| `settings.h` | every setting, its config-file key, its widget and its tab, in one table. |
| `ui.cpp/.h` | the widget set the settings window is drawn with. Text only, on purpose. |
| `joincode.h` | an address and port as ten letters, for reading out loud. |
| `brand.h` | the mark at the top of each window, and the version number -- the one place it lives. |
| `mathconst.h` | `xr::kPi`, because `M_PI` is not standard C++ and MSVC knows it. |

## Threads

X-Plane calls plugins on its main thread, and only that thread may touch the
sim. Everything else that has to wait for something runs elsewhere:

- **main thread** (`flightLoop` in `main.cpp`, every frame): reads datarefs,
  sends our position, drains the inbox of packets the network thread queued,
  drives XPMP2 and the windows.
- **network thread** (`netLoop`): owns the receive side of the socket. Voice
  frames go straight to the mixer; everything else is queued for the main
  thread. It also opens the socket, because resolving a host name can block
  for seconds.
- **audio threads** (miniaudio's, in `voice.cpp`): capture and playback
  callbacks. They must never block, so they only ever `try_lock`.
- **relay thread** (`server.cpp`): the hosted server, when hosting is on.
- **UPnP worker** (`upnp.cpp`): talks to the router, one job at a time.
- **update worker** (`update.cpp`): one HTTP request, then gone.

Shared state is either atomic, or behind a mutex held for microseconds, or
touched by one thread only -- and each global in `main.cpp` says which.

## The wire protocol

Packed little-endian UDP, defined twice -- `plugin/src/protocol.h` and
`server/protocol.py` -- and the two **must stay in sync**. CI compiles
`tools/check_sizes.cpp` and diffs its answers against
`tools/check_sizes.py`, so a struct that grew on one side fails the build.

Every datagram is a 12-byte `Header` (magic, type, protocol version, payload
length, session id) and a payload. The server refuses a login from any other
protocol version with a reason, and everything that is not a login needs the
session id the server handed out. Nothing is ever larger than 1200 bytes,
because WireGuard VPNs hand you a 1280-byte link.

To add a packet type: a `PT_*` value and a payload struct in both protocol
files; the sizes in `check_sizes.cpp` and `check_sizes.py`; a handler in
`Core::onPacket` and in `XRadioServer._dispatch`; a handler in
`pumpNetwork` in `main.cpp`; and a scenario in `tools/test_server_parity.py`
that proves both servers do the same thing with it. A new type that old
clients can simply drop does not need a version bump; a change to an
existing packet does.

## Two servers, one behaviour

The relay exists twice because a pilot hosting from inside the sim should
not need Python, and a VPS should not need X-Plane. `tools/test_server_parity.py`
starts both, drives the same scenarios at them over real UDP and compares
the transcripts byte for byte, then checks the answers are the right ones.
Fix a rule in one server and the test tells you about the other.

## Settings

`settings.h` has one table, `describe()`, listing every setting with its
config key, label, widget kind, tab and limits. The config file reader and
writer and the settings window all walk that table, so a new setting is one
line there plus the member in `Settings`. The settings test walks it too and
checks that nothing is saved-but-not-loaded or drawn over its own value.

## Tests, and how to run them

Everything in `.github/workflows/build.yml` runs on every push. Locally:

```bash
bash tools/check_portability.sh        # the mistakes that only fail on one platform
python3 tools/test_server.py           # the Python server, protocol and routing
cmake -B build-tests -DXRADIO_BUILD_TESTS=ON -DXRADIO_USE_XPMP2=OFF \
      -DXPLANE_SDK=$PWD/tools/fake_sdk && cmake --build build-tests -j
./build-tests/settings_test            # and voice_test, hosting_test, placement_test ...
python3 tools/test_server_parity.py --cpp ./build-tests/xradio_server
```

The README's *Tests* section lists them all, including the fake router, the
fake release site and the two fuzzers. On Windows, use Visual Studio's
compiler with `-G Ninja`; the README says what differs there.

The plugin tests work because `tools/harness/xplm_stub.cpp` implements just
enough of the XPLM API -- datarefs from a table, windows that record what
they drew -- for the real `main.cpp` to run as an ordinary executable. A test
therefore reads the window's text back and clicks the row a label was drawn
on, which is exactly what a pilot does.

## Rules that keep it portable

`tools/check_portability.sh` enforces these in seconds; they are the ones
that pass on Linux and macOS and fail on Windows ten minutes later:

- `xr::kPi` from `mathconst.h`, never `M_PI`.
- POSIX headers only inside `#ifndef _WIN32`; Windows APIs only with the
  header that declares them (`SIO_UDP_CONNRESET` is in `<mstcpip.h>`).
- No variable-length arrays; `<algorithm>` wherever `std::min`/`std::max`
  are used.
- The UI is `XPLMDrawString` only: no OpenGL in window callbacks.
- X-Plane 11.50 is the floor. The build enables the SDK up to `XPLM303`
  and deliberately not `XPLM400`, so an X-Plane 12-only API cannot creep in;
  what 12 adds (region weather) is found by dataref at run time.

## Cutting a release

`bash release.sh` (or `release.bat`) asks for the version, runs every check
this machine can run, sets the number in `brand.h`, writes the source zip and
a release-notes skeleton into `dist/`. The binaries pilots install come from
the CI run's `XRadio-all-platforms` artifact. Everyone in a flight must run
the same build, so the version is checked at login.
