# Air traffic control over XRadio

XRadio v0.6.0 lets a controller join a flight. Not as an aircraft: as a station
on a frequency that sees everybody, takes flight plans, publishes an ATIS and
-- if it is a program such as [TowerTrainer](https://github.com/Anviktor2411/TowerTrainer)
-- fills the airport with computer-flown traffic that every pilot sees and hears.

This page is in two halves: what a pilot does, and what a program that wants
to be ATC has to send.

## For pilots

1. **Find the controller.** With a station in the flight the XRadio window shows
   an *ATC online* block: callsign, position and frequency. Click a station and
   COM1 is tuned to it; click an ATIS entry and COM2 is tuned to that. A new
   station is also announced in the radio log and, with the window shut, as a
   notice in the corner.
2. **Get the ATIS.** Tune the ATIS frequency on either radio. The information
   letter and the text appear under the ATC block, and the log notes every new
   letter. Report the letter you have when you call.
3. **File a flight plan.** Settings → ATC: rules (I, V, Y, Z), departure,
   destination, alternate, cruise altitude, route and remarks, then *File flight
   plan*. The plan goes to every controller in the flight. Their answer comes
   back to the window: *acknowledged*, a *squawk* (set it on the transponder --
   the window says when it matches) or a *clearance*. The fields are kept for
   the next flight; *Cancel flight plan* withdraws it.
4. **Talk.** Voice and text on the frequency work as always, push-to-talk and
   the Say field. A call from a controller is green in the log; a message meant
   only for you (`@YOURCALLSIGN …`) is amber. Both are read out by the sim's
   voice unless you switch that off on the ATC tab.
5. **Computer-flown traffic** is tagged *AI* in the traffic list. The ATC tab
   has a cap on how many are drawn (nearest first; 40 by default) and a switch
   to leave them out of the list. They are on TCAS and on the radio like anyone
   else.

## For an ATC program

Everything is UDP to the relay server, protocol v6, the structs in
`plugin/src/protocol.h` and `server/protocol.py`. `tools/fake_client.py` does
all of it from the command line and is the quickest way to see the packets.

### Logging in as a station

`PT_LOGIN` with `flags = LF_ATC`. The callsign names the position:
`EETN_TWR`, `EETN_GND`, `EETN_DEL`, `EETN_APP`, `EETN_CTR` -- the airport before
the underscore and the position after it are what the ATC list shows. Send
`PT_POSITION` like a pilot, from the tower's position (`onGround = 1`,
altitude of the cab): the horizon for voice and text is worked out from it. Put
the position's frequency on COM1 and, if the station should hear guard without
being tuned to it, 121.500 on COM2 with `rxMask = RX_COM1 | RX_COM2`.

A station is never relayed as traffic and is not on anybody's TCAS. It does
receive `PT_TRAFFIC` (pilots and injected aircraft within 80 nm), every
`PT_FLIGHTPLAN` filed in the flight (and all existing plans on login), the
roster, the ATC list and text and voice on its frequencies.

    python3 tools/fake_client.py --atc --callsign EETN_TWR --com1 135905 \
        --lat 59.413 --lon 24.833 --alt-ft 150 \
        --atis "Tallinn information Charlie, runway 26, wind 250 degrees 8 knots" \
        --atis-freq 127905 --atis-letter C --ack-plans 4521

### Driving aircraft: the injector

A second login with `flags = LF_INJECTOR` (one per program is enough). Then
`PT_AI_TRAFFIC` every 200 ms: a `TrafficHeader` followed by up to ten
`TrafficEntry` records -- the same records `PT_TRAFFIC` delivers -- with the
program's own local number (1…65535) in `sessionId`. The server validates each
entry like a position report, keeps the set alive for 5 s after the last
refresh, and relays the aircraft to everyone in range as ordinary traffic under
a flight-wide id (`aiSid(injectorSid, local)`: the injector's session in the high
half, the local number in the low half, top bit set). An entry with
`reserved & AI_REMOVE` drops that aircraft at once. One injector may drive 60
aircraft; nearest first is the rule on the way out, as for any client.

An injected aircraft speaks by the injector sending `PT_TEXT` or `PT_VOICE`
with `fromSession = aiSid(...)` and the frequency in the header; the server
relays it in that aircraft's callsign and position (and shows it keyed on
everybody's traffic list while voice is coming in). The injector itself never
hears its own aircraft back.

### Flight plans

A pilot files `PT_FLIGHTPLAN` with `sessionId = 0`; the server fills in the
pilot's session and callsign, clears the squawk, and relays it to every
station. A station answers with the same payload, `sessionId` naming the
pilot, `status = FP_ACK` (with a squawk) or `FP_CLEARED` (the clearance text in
`remarks`) or `FP_CANCELLED`; the server passes it to that pilot and to the
other stations, and keeps the latest copy so a station that joins later gets
it on login.

### ATIS

A station sends `PT_ATIS`: frequency, airport, information letter and up to
600 bytes of text. The server offers it to every pilot who has that frequency
tuned, once per letter, and again to everyone tuned whenever the letter or the
text changes; the frequency appears in the ATC list as `EETN_ATIS`. Send it
again whenever it changes, and at least every few minutes. The voice of the
ATIS, if you want one, is ordinary `PT_VOICE` from a station tuned to the ATIS
frequency.

### The ATC list

`PT_ATC_LIST` goes from the server to everyone whenever a station joins, leaves
or retunes, and every five seconds besides: one `AtcEntry` per station
(session, callsign, airport, frequency, kind) plus one per published ATIS.

### Tests

`python3 tools/test_server.py` drives all of the above against the Python
server over real UDP; `python3 tools/test_server_parity.py --cpp <xradio_server>`
runs the same scenarios against the built-in C++ server and diffs the two;
`python3 tools/check_sizes.py --compare <check_sizes>` holds the struct
definitions together.
