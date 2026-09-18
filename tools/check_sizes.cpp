// Prints the size of every wire struct so it can be compared against
// server/protocol.py. Build:  g++ -std=c++17 -Iplugin/src tools/check_sizes.cpp -o /tmp/cs
#include <cstdio>
#include "protocol.h"

int main() {
    printf("HEADER %zu\n",        sizeof(xr::Header));
    printf("LOGIN %zu\n",         sizeof(xr::LoginPayload));
    printf("LOGIN_ACK %zu\n",     sizeof(xr::LoginAckPayload));
    printf("POSITION %zu\n",      sizeof(xr::PositionPayload));
    printf("TRAFFIC_HDR %zu\n",   sizeof(xr::TrafficHeader));
    printf("TRAFFIC_ENTRY %zu\n", sizeof(xr::TrafficEntry));
    printf("TEXT_HDR %zu\n",      sizeof(xr::TextHeader));
    printf("VOICE_HDR %zu\n",     sizeof(xr::VoiceHeader));
    return 0;
}
