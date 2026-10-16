// Prints the size of every wire struct so it can be compared against
// server/protocol.py -- and the radio model's answers on a table of cases, to
// be compared against server/server.py's (the two servers route by them).
// Build:  g++ -std=c++17 -Iplugin/src tools/check_sizes.cpp -o /tmp/cs
#include <cstdio>
#include <cstring>
#include "protocol.h"
#include "radio.h"

namespace {
using xr::radio::Node;

Node aircraft(double lat, double lon, double altMslM, double aglM) {
    Node n;
    n.lat = lat; n.lon = lon; n.altMslM = altMslM;
    n.heightFt = xr::radio::aircraftHeightFt(aglM);
    return n;
}

Node station(double lat, double lon, double altMslM, uint8_t kind) {
    const xr::radio::StationDefaults d = xr::radio::stationDefaults(kind);
    Node n;
    n.lat = lat; n.lon = lon; n.altMslM = altMslM;
    n.heightFt = d.antennaM * xr::radio::kFtPerM;
    n.atc = true; n.rangeNm = d.rangeNm; n.ceilingFt = d.ceilingFt;
    return n;
}

void radioLine(const char* name, const Node& tx, const Node& rx, uint32_t khz, bool ducting) {
    const xr::radio::Link l = xr::radio::link(tx, rx, khz, ducting);
    printf("RADIO_%s %.4f %.2f %.3f %.3f\n", name, l.quality, l.levelDbm, l.distNm, l.coverageNm);
}

xr::WeatherPayload warmStill(int32_t dateDays, float zuluSec) {
    xr::WeatherPayload w;
    memset(&w, 0, sizeof(w));
    w.dateDays = dateDays; w.zuluTimeSec = zuluSec;
    w.seaLevelTempC = 24.f; w.seaLevelPressurePa = 102200.f; w.rainPercent = 0.f;
    w.windSpeedMs[0] = 2.5f;
    return w;
}

// The cases, in the order tools/check_sizes.py prints them.
void radioCases() {
    using namespace xr;
    // Innsbruck's apron (1900 ft): two aircraft on the ground 2.4 nm apart, and 10 nm
    const Node lowiA = aircraft(47.2602, 11.3439, 581.0, 0.0);
    radioLine("APRON_LOWI", lowiA, aircraft(47.3002, 11.3439, 581.0, 0.0), 120100, false);
    radioLine("APRON_LOWI_10NM", lowiA, aircraft(47.4269, 11.3439, 581.0, 0.0), 120100, false);
    // ...and the same two seen through a v5 client's eyes: on the ground, 0 above it
    radioLine("APRON_LOWI_V5", lowiA,
              aircraft(47.3002, 11.3439, 581.0, radio::effectiveAglM(false, 0.0, true, 581.0)), 120100, false);
    // a v5 aircraft in the air counts its altitude, as before
    radioLine("V5_AIRBORNE", aircraft(57.85, 27.02, 914.4, radio::effectiveAglM(false, 0.0, false, 914.4)),
              aircraft(59.50, 27.00, 914.4, radio::effectiveAglM(false, 0.0, false, 914.4)), 122800, false);
    // Tallinn tower and a 10 nm final at 3000 ft, both ways
    const Node twr = station(59.4133, 24.8328, 70.0, ATC_TWR);
    const Node fin = aircraft(59.4133, 24.5054, 914.4, 873.0);
    radioLine("TWR_TO_FINAL", twr, fin, 118100, false);
    radioLine("FINAL_TO_TWR", fin, twr, 118100, false);
    // on the ground 20 nm out: past the horizon (18.9 nm), inside the tower's 25 nm
    radioLine("TWR_SERVICE_VOLUME", twr, aircraft(59.7466, 24.8328, 40.0, 0.0), 118100, false);
    // ...but above its 4000 ft ceiling only the horizon counts
    radioLine("TWR_ABOVE_CEILING", twr, aircraft(59.7466, 24.8328, 3000.0, 2960.0), 118100, false);
    // FL350 and a ground station 200 nm and 260 nm away
    radioLine("FL350_TO_TWR_200NM", aircraft(62.7466, 24.8328, 10668.0, 10600.0), twr, 132800, false);
    radioLine("TWR_TO_FL350_200NM", twr, aircraft(62.7466, 24.8328, 10668.0, 10600.0), 132800, false);
    radioLine("FL350_TO_TWR_260NM", aircraft(63.7466, 24.8328, 10668.0, 10600.0), twr, 132800, false);
    // a centre's area: FL100 180 nm from its site
    radioLine("CTR_FL100_180NM", station(59.4133, 24.8328, 70.0, ATC_CTR),
              aircraft(62.4133, 24.8328, 3048.0, 3000.0), 132800, false);
    // two aircraft at 500 ft 80 nm apart over the sea, with and without a duct
    const Node seaA = aircraft(59.60, 22.00, 152.4, 152.4), seaB = aircraft(59.60, 24.6350, 152.4, 152.4);
    radioLine("SEA_80NM", seaA, seaB, 121500, false);
    radioLine("SEA_80NM_DUCT", seaA, seaB, 121500, true);
    // whether the flight's weather ducts, at four hours of one warm still day, and on a cold one
    for (float zulu : {0.f, 3600.f * 7, 3600.f * 18, 3600.f * 21})
        printf("DUCTING_%d %d\n", (int)(zulu / 3600.f), radio::ductingFrom(warmStill(19500, zulu)) ? 1 : 0);
    xr::WeatherPayload cold = warmStill(19500, 3600.f * 18);
    cold.seaLevelTempC = 8.f;
    printf("DUCTING_COLD %d\n", radio::ductingFrom(cold) ? 1 : 0);
}
}  // namespace

int main() {
    printf("HEADER %zu\n",        sizeof(xr::Header));
    printf("LOGIN %zu\n",         sizeof(xr::LoginPayload));
    printf("LOGIN_ACK %zu\n",     sizeof(xr::LoginAckPayload));
    printf("LOGIN_REJECT %zu\n",  sizeof(xr::LoginRejectPayload));
    printf("POSITION %zu\n",      sizeof(xr::PositionPayload));
    printf("TRAFFIC_HDR %zu\n",   sizeof(xr::TrafficHeader));
    printf("TRAFFIC_ENTRY %zu\n", sizeof(xr::TrafficEntry));
    printf("TEXT_HDR %zu\n",      sizeof(xr::TextHeader));
    printf("VOICE_HDR %zu\n",     sizeof(xr::VoiceHeader));
    printf("WEATHER %zu\n",       sizeof(xr::WeatherPayload));
    printf("ROSTER_HDR %zu\n",    sizeof(xr::RosterHeader));
    printf("ROSTER_ENTRY %zu\n",  sizeof(xr::RosterEntry));
    printf("FLIGHTPLAN %zu\n",    sizeof(xr::FlightPlanPayload));
    printf("ATIS_HDR %zu\n",      sizeof(xr::AtisPayload));
    printf("ATC_LIST_HDR %zu\n",  sizeof(xr::AtcListHeader));
    printf("ATC_ENTRY %zu\n",     sizeof(xr::AtcEntry));
    printf("AIRCRAFT %zu\n",      sizeof(xr::AircraftPayload));
    printf("TCAS %zu\n",          sizeof(xr::TcasPayload));
    // Not a struct, but the same class of mistake: a packet budget or an
    // entry cap that drifts apart between the two servers shows up as
    // traffic that thins out for clients of one of them.
    printf("MAX_PACKET %d\n",      xr::kMaxPacket);
    printf("MAX_TRAFFIC_ENTRIES %d\n", xr::kMaxTrafficEntries);
    printf("MAX_ROSTER_ENTRIES %d\n",  xr::kMaxRosterEntries);
    printf("PT_ROSTER %d\n",      (int)xr::PT_ROSTER);
    printf("GUARD_KHZ %u\n",      (unsigned)xr::kGuardKhz);
    printf("PROTO_VERSION %d\n",  (int)xr::kProtoVersion);
    printf("MAX_ATC_ENTRIES %d\n", xr::kMaxAtcEntries);
    printf("MAX_ATIS_BYTES %d\n",  xr::kMaxAtisBytes);
    printf("PT_AI_TRAFFIC %d\n",  (int)xr::PT_AI_TRAFFIC);
    printf("PT_FLIGHTPLAN %d\n",  (int)xr::PT_FLIGHTPLAN);
    printf("PT_ATIS %d\n",        (int)xr::PT_ATIS);
    printf("PT_ATC_LIST %d\n",    (int)xr::PT_ATC_LIST);
    printf("PT_AIRCRAFT %d\n",    (int)xr::PT_AIRCRAFT);
    printf("PT_TCAS %d\n",        (int)xr::PT_TCAS);
    printf("LF_ATC %d\n",         (int)xr::LF_ATC);
    printf("LF_INJECTOR %d\n",    (int)xr::LF_INJECTOR);
    printf("AI_SID %u\n",         (unsigned)xr::aiSid(7, 12));
    printf("MIN_PROTO_VERSION %d\n", (int)xr::kMinProtoVersion);
    printf("POSITION_V5 %d\n",    xr::kPositionSizeV5);
    printf("TRAFFIC_ENTRY_V5 %d\n", xr::kTrafficEntrySizeV5);
    printf("MAX_TRAFFIC_ENTRIES_V5 %d\n", xr::kMaxTrafficEntriesV5);
    printf("AL_DUCTING %d\n",     (int)xr::AL_DUCTING);
    printf("AE_POSITION %d\n",    (int)xr::AE_POSITION);
    radioCases();
    return 0;
}
