// Prints the size of every wire struct so it can be compared against
// server/protocol.py. Build:  g++ -std=c++17 -Iplugin/src tools/check_sizes.cpp -o /tmp/cs
#include <cstdio>
#include "protocol.h"

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
    printf("LF_ATC %d\n",         (int)xr::LF_ATC);
    printf("LF_INJECTOR %d\n",    (int)xr::LF_INJECTOR);
    printf("AI_SID %u\n",         (unsigned)xr::aiSid(7, 12));
    return 0;
}
