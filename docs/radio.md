# The radio model (v0.6)

How far a transmission reaches in XRadio, and how it sounds when it gets there.
One model, written twice: `plugin/src/radio.h` (the server built into the plugin
routes by it, the plugin turns it into sound) and `server/server.py` (the
dedicated server routes by it). `tools/check_sizes.py` makes the two work out a
table of links and compares the answers digit for digit, so a station cannot be
heard in a flight on one server and be silent on the other.

In short:

1. The **horizon** is worked out from each aerial's height **above the ground**,
   not from the altitude, and is never under **5 nm**.
2. An **ATC station** also covers its **service volume** (a tower 25 nm and
   4000 ft, approach 60 nm, a centre its whole area), horizon or not.
3. Inside that, the **signal** comes from a link budget: transmitter power, less
   free space path loss, against the receiver's noise -- fading out towards the
   edge of the coverage, where it ends.
4. The plugin, which has the scenery, takes off what the **terrain** on the path
   costs: a ridge in the way makes a station weak and noisy, or silent.
5. Two transmissions at once fight it out by **level** (the capture effect).
6. Optionally, on a warm and still evening, **tropospheric ducting** carries VHF
   far past the horizon over the sea.

## 1. Heights and the horizon

    h  = max(0, height above the ground) + aerial          (feet)
    horizon = 1.23 × (√h₁ + √h₂)   nm,   never less than 5 nm

The 1.23 is the radio horizon over an earth bent by refraction to 4/3 of its
real radius. What matters is the height above the ground: from the altitude,
two aircraft on Innsbruck's apron (1900 ft) got a 107 nm horizon -- today they
get 7.7.

| who | height above the ground | aerial |
|---|---|---|
| an aircraft (XRadio 0.6) | the sim's own (`sim/flightmodel/position/y_agl`), 0 on the ground | 3 m (10 ft), a blade on the fuselage |
| an aircraft from a v5 client, or a 108-byte injected entry | 0 on the ground; **in the air its altitude** (nothing better is known -- what every version before v6 used) | 3 m |
| an ATC station | -- | what it sends (`aglM`), else its facility's: |

| facility | antenna above the terrain around it | service volume |
|---|---|---|
| DEL, GND | 30 m | 10 nm, up to 2000 ft above the station |
| TWR | 45 m | 25 nm, up to 4000 ft |
| APP / DEP | 300 m (a remote site) | 60 nm, up to 25 000 ft |
| CTR | 600 m (remote sites) | 250 nm (its area), up to 65 000 ft |
| ATIS | 45 m | 60 nm, up to 20 000 ft |
| anything else | 30 m | none |

**The 5 nm floor.** Two aircraft on the ground already see 7.7 nm from their
own aerials, so the floor only matters in the corners: a sunken apron, a terrain
probe that put the ground a metre high. Five miles is more than any airport is
across, so everybody on one airport always hears everybody else. It used to be
15 nm, which with the altitude in the sums hid how short a real ground-to-ground
range is.

## 2. Service volumes

A facility places its antennas to cover its airspace -- approach and centre on
remote sites, several of them -- so a station reaches an aircraft inside its
service volume whatever the horizon says:

    coverage = max(horizon, service radius)    for an aircraft below the ceiling
    coverage = horizon                         otherwise, and between two aircraft

Inside the volume the signal is full strength to its last tenth, and the plugin
does not shadow it behind terrain. The volume is the station's: two pilots 20 nm
apart on the ground still need their own horizon to hear each other. A station
can send its own radius and ceiling (`svcRangeNm`, `svcCeilingFt`); TowerTrainer
sends 0, which means its facility's.

## 3. The signal: a link budget

    received (dBm) = transmitter − FSPL − terrain
    FSPL (dB)      = 32.45 + 20·log10(f MHz) + 20·log10(d km)
    SNR            = received − noise

| | |
|---|---|
| transmitter | aircraft 10 W (40 dBm), ATC station 25 W (44 dBm) |
| noise | −113 dBm: a 25 kHz channel with a 7 dB noise figure (a COM's −107 dBm sensitivity is 6 dB over it) |
| f | the transmission's own frequency, kept inside 100–160 MHz |

Then

    quality = smooth((SNR − 3) / 30) × fade        (smooth: 0 below 0, 1 above 1, smoothstep between)
    fade    = 1 − smooth((d/horizon − 0.6) / 0.4)   ...or 1 − smooth((d/radius − 0.9) / 0.1) inside a service
                                                       volume, whichever is larger; 0 past both

SNR 3 dB is nothing worth hearing, 33 dB is clean. The fade is the path grazing
the earth's bulge and then going under it: clean to 60 % of the horizon, nothing
at it. A transmission whose quality is above 0 is what the server relays; the
plugin's voice code turns the quality into the noise floor, the flutter and the
20 ms dropouts it already had (`plugin/src/voice.cpp`), and the squelch knob is
the quality a station needs to open the receiver at all.

At VHF power the free space loss never ends a link on its own -- the horizon
does -- but it makes a long link noisier than a short one: FL350 to a tower
200 nm away is readable, never clean.

## 4. Terrain (the plugin)

The server has no scenery, so it relays by the horizon alone. The plugin samples
the straight line between its aerial and each station's -- a terrain probe
(`XPLMProbeTerrainXYZ`) every kilometre, at most 240 on one path -- with the
earth's bulge for a 4/3 radius:

    bulge(d₁, d₂) = d₁·d₂ / (2·k·R),   k = 4/3, R = 6371 km
    h  = terrain + bulge − line between the two aerials        (> 0: in the way)
    ν  = h · √(2·D / (λ·d₁·d₂))                                 (knife edge, λ = c/f)
    loss (dB) = 6.9 + 20·log10(√((ν − 0.1)² + 1) + ν − 0.1)     (ITU-R P.526, ν > −0.78)

The worst obstacle on the path decides. Its loss comes off the received level,
and once it actually blocks the line (ν > 0) the quality is also capped:

    cap = 0.45 × (1 − ν / 10)       (0.45 grazing: weak and noisy; ν ≥ 10: nothing at all)

A ridge 100 m above the line half way along a 40 km path costs about 15 dB and
caps the signal at weak and noisy. A ridge 1500 m above it (ν ≈ 14) lets nothing
through. Where X-Plane has no scenery loaded the probe misses, and nothing is
assumed to be in the way.

It must not cost frames. The sampler (`plugin/src/shadow.cpp`) spends at most
32 probes and 0.4 ms a frame, one path at a time -- stations that are talking or
tuned first -- so a 100 km path takes three or four frames. An answer is kept
until either end has moved 300 m (or 30 m up or down) and 2.5 s have passed; a
change of frequency is worked out again from the same profile without a probe.

## 5. Two at once: the capture effect

When two stations transmit together, the receiver compares their levels:

* **more than 6 dB apart**: the stronger captures the receiver. It is heard; the
  weaker is a whine underneath -- the beat of the two carriers, fainter dB for
  dB the further down it is -- and its words are lost;
* **within 6 dB**: nobody captures it. Both garble, chopped by the beat between
  them, under the full heterodyne squeal.

## 6. Tropospheric ducting (optional, off by default)

On a warm, still evening a temperature inversion over the sea can trap VHF and
carry it far beyond the horizon. Whoever runs the server can allow it:
`server.py --ducting`, `xradio_server --ducting` (or `XRADIO_DUCTING=1`), or in
the plugin's Host tab *Rare tropospheric ducting*. It then comes from the
weather the flight shares, never from one pilot's sim:

* warm (sea-level temperature ≥ 20 °C), high pressure (≥ 1015 hPa), little wind
  (≤ 6 m/s at the surface), dry;
* and even then only about a third of the hours, picked from the sim's date and
  hour so every server and every radio agrees which.

In a duct, links between two aerials below 6000 ft reach twice the horizon. The
server cannot see the sea, so it relays the whole extension and says so in the
ATC list (`AL_DUCTING`); the plugin hears it only over a path that is at least
70 % water. When it begins and ends the radio log says so.

## The server and the plugin agree

The server relays a transmission when the model gives a quality above 0 -- the
same function the receiving plugin works the signal out with, from the same
numbers: each aircraft's height above the ground travels in the traffic
entries (`TrafficEntry.aglM`, the server's own figure, fallbacks included), and
each station's position, antenna and service volume in the ATC list
(`AtcEntry`). Three things can still differ, all on purpose:

* **terrain** -- only the plugin has it, and it only ever takes away;
* **the squelch** -- each pilot's own knob;
* **timing** -- positions a fraction of a second apart. The signal fades to
  nothing at the edge of the coverage, so a disagreement there is inaudible.

## Worked examples

From `tools/check_sizes.py`'s table, which both servers produce identically:

| link | distance | coverage | quality |
|---|---|---|---|
| two aircraft on Innsbruck's apron (1900 ft) | 2.4 nm | 7.7 nm | 1.00 clean |
| the same, 10 nm down the valley | 10 nm | 7.7 nm | 0 -- nothing (it was 107 nm from the altitude) |
| Tallinn tower → a 10 nm final at 3000 ft | 10 nm | 81 nm | 1.00 |
| the tower → an aircraft on the ground 20 nm out | 20 nm | 25 nm (service volume; the horizon is 18.9) | 1.00 |
| FL350 → a tower 200 nm away | 200 nm | 244 nm | 0.38 noisy |
| the tower → FL350 200 nm away (25 W) | 200 nm | 244 nm | 0.42 |
| FL350 260 nm away | 260 nm | 244 nm | 0 |
| a centre → FL100 180 nm out | 180 nm | 250 nm (its area) | 0.99 |
| two aircraft at 500 ft over the sea | 80 nm | 56 nm | 0 |
| the same in a duct | 80 nm | 111 nm | 0.78 |
| two v5 aircraft at 3000 ft (altitude counted) | 99 nm | 135 nm | 0.74 |

## On the wire (protocol v6)

| record | field | |
|---|---|---|
| `PositionPayload` | `aglM`, `svcRangeNm`, `svcCeilingFt` | 80 bytes. A 72-byte report (v5) is still taken, height unknown |
| `TrafficEntry` | `aglM` | 112 bytes to a v6 client, 108 to a v5 one; an injector may send either |
| `AtcEntry` | `flags` (`AE_POSITION`), `lat`, `lon`, `altMslM`, `antennaM`, `rangeNm`, `ceilingFt` | 64 bytes |
| `AtcListHeader` | `flags` (`AL_DUCTING`) | |

A v5 client (XRadio 0.5) still logs in: it gets v5 headers, 108-byte traffic
entries and none of v6's own packets, and is routed as 0 above the ground on
the ground and by its altitude in the air.

## Tests

| what | where |
|---|---|
| the model, Python and C++ digit for digit | `tools/check_sizes.py --compare` |
| the model in C++: horizons, service volumes, the link budget, ducting, a ridge on `tools/fixtures/radio_profile_ridge.txt`, the sampler's probe and time budget and its cache | `tools/harness/radio_test.cpp` |
| the same in Python, and routing over real UDP: Innsbruck's apron, a tower and its final, FL350, v5 clients, ducting | `tools/test_server.py` |
| both servers routing every one of these the same way | `tools/test_server_parity.py` |
| the capture effect: who wins at 15 dB, the other way round, garble at 2 dB, the whine against the squeal | `tools/harness/voice_test.cpp` |
