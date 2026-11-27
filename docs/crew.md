# Shared cockpit and the intercom (0.7)

Two or more pilots in one aircraft, each in their own X-Plane: a captain and a
first officer, an instructor and a student, a friend riding along. XRadio
gives them what a shared cockpit needs on the radio side:

- a **private intercom** between everyone aboard, separate from COM1 and COM2;
- one set of **radios for the aircraft**: the crew hear what its COMs hear,
  from where it is, and a copilot transmits on them *as the aircraft*;
- a **radio panel in step**: the crew's own sims follow the aircraft's
  frequencies, and a knob a copilot turns retunes the aircraft;
- the crew are **not aircraft of their own** in anybody's sky or TCAS.

What it does not do is fly the aircraft twice: XRadio does not synchronise
the flight model, the switches or the instruments. For a cockpit where both
pilots fly the same aeroplane, use a shared-cockpit add-on (SmartCopilot and
the like) for the aircraft and XRadio for the radios -- they do not get in
each other's way. Without one, the crew's own sims can sit anywhere: an
observer parked at the airport still hears exactly what the aircraft hears.

## Setting it up

**The pilot** (the aircraft): *Settings > Crew* in the window, or
*Settings > Connection* in the text window --

- Shared cockpit: **my cockpit takes a crew** (`crewmode = pilot`)
- Crew code: anything, or empty to let anyone in the flight join

**Everyone else**:

- Shared cockpit: **I join a cockpit** (`crewmode = join`)
- Pilot to join: the pilot's callsign (`crewjoin = ESNA12`)
- Role: **copilot** -- hears the radios, talks on them as the aircraft, may
  retune them -- or **observer** -- hears the radios, talks on the intercom
  only (`crewrole = copilot` / `observer`)
- Crew code: the one the pilot set (`crewcode`)
- My radios follow the aircraft's (`crewsync = yes`, the default)

Apply, and XRadio asks the server until it is so -- the pilot can open the
cockpit after the crew have asked, in either order. The Radio tab shows who
is aboard; the text window has a line for it; the overlay shows **IC**.

Four aboard at most, the pilot included. One pilot can have one cockpit open,
and somebody with their own cockpit open cannot sit in another (set it to
*join* first).

## The intercom

Hold **Intercom** on the Radio tab, the intercom key (*Settings > Crew*), or
bind the command **`xradio/intercom`** to a yoke button. Text typed after
`/ic ` in the Messages tab goes on the intercom too.

The intercom is a wire, not a radio: no range, no squelch, no static, no
capture -- it is heard clean (through a headset's band, with *Radio sound*
on) at its own **Intercom volume**, and it is heard while you transmit on the
radio. Talking on the intercom does not stop you hearing the COMs. With both
push-to-talks down, the radio wins, as on a real audio panel. Muting a pilot
(Hosting) never touches their cockpit's intercom: it is nobody else's
business.

## The aircraft's radios

- **Hearing.** The server routes to the crew what the aircraft's radios
  receive: the aircraft's frequencies and receive buttons, the aircraft's
  position for the VHF horizon. Each crew member's own volume knobs are their
  own headset's.
- **Transmitting.** A copilot's push-to-talk keys the aircraft's selected COM.
  Everyone outside hears it from the aircraft, under its callsign and from
  where it is, with its [TX] flag; the rest of the crew hear it in the
  cockpit, from whoever is talking. An observer has no transmitter.
- **One transmitter.** An aircraft has one transmitter per radio, so the first
  of the crew to key holds it until they let go -- a second crew member
  keying at the same time is not on the air. (Two people talking into one
  radio at once is unreadable anyway; this way it is the first one, clean.)
- **The panel.** The pilot's sim sends its panel -- COM1/COM2 active and
  standby, the transmit selector, the receive buttons, which radios have
  power -- whenever it changes and every two seconds. A crew member's sim
  follows it (with *My radios follow the aircraft's* on). A copilot who turns
  a knob in their own cockpit, or picks a quick-tune slot, asks the pilot's
  sim to make the change; the pilot sees "ESCOP1 set COM1 121.800" in the radio
  log. An observer's own knobs are put back.
- **Power.** As crew, the aircraft's radios are the ones with or without power:
  a copilot in a cold-and-dark sim still talks on the aircraft's radio.

## The protocol

All of it is one new packet type, **`PT_CREW` (19)**, and one sentinel
frequency, **`kIntercomKhz` (1)**, on the existing `PT_VOICE` / `PT_TEXT`.
Nothing existing changed, so no protocol bump: a pre-0.7 client never sends
or receives any of it. `plugin/src/protocol.h` has the records:

| op | from → to | carries |
|---|---|---|
| `CREW_OPEN` | pilot → server | the crew code |
| `CREW_CLOSE` | pilot → server | -- everyone aboard is sent `CREW_REFUSED` / `CREW_CLOSED` |
| `CREW_JOIN` | crew → server | the pilot's callsign, the code, the role |
| `CREW_LEAVE` | crew → server | -- |
| `CREW_STATE` | server → everyone aboard | the aircraft, the receiver's role, `CrewMember` records (count 0: in no cockpit). On every change and every five seconds |
| `CREW_REFUSED` | server → would-be crew | why: no such pilot, not open, wrong code, full, closed, busy |
| `CREW_PANEL` | pilot → server → crew | a `CrewPanel`; kept, so crew who join later get it at once |
| `CREW_TUNE` | copilot → server → pilot | a `CrewPanel` where 0 / 0xFF / a negative volume mean "leave it" |

Routing on the server (`_listeners` / `listeners()`, `_on_voice` / `onVoice`):

- intercom (`kIntercomKhz`): to everyone in the sender's cockpit but the
  sender; no range, never muted;
- a crew member's COM voice or text: validated against the **aircraft's**
  radios, relayed to the outside **as the aircraft** (its session id and
  callsign; voice numbered by the server, `voiceSeqOut`, so the aircraft's one
  stream never runs backwards), and to the rest of the crew from the speaker;
- listening: a crew member hears what their aircraft's radios hear, from the
  aircraft's position;
- traffic: crew are not sent as aircraft, and do not see the one they are in.

Both servers do all of this identically: `tools/test_server_parity.py`'s
*crew* scenario drives it at both and compares the bytes, and
`tools/harness/crew_harness.cpp` runs the real plugin as copilot and as pilot
against fake pilots (`tools/fake_client.py --crew-open` / `--crew-join`).
