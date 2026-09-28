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
    // Not a struct, but the same class of mistake: a packet budget or an
    // entry cap that drifts apart between the two servers shows up as
    // traffic that thins out for clients of one of them.
    printf("MAX_PACKET %d\n",      xr::kMaxPacket);
    printf("MAX_TRAFFIC_ENTRIES %d\n", xr::kMaxTrafficEntries);
    printf("GUARD_KHZ %u\n",      (unsigned)xr::kGuardKhz);
    return 0;
}
