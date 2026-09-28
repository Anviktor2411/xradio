#!/usr/bin/env python3
"""A router that speaks NAT-PMP, in each of its moods.

The fallback path only matters on routers with UPnP switched off, which is
exactly the setup nobody has to hand when they need it. So here is one.

    python3 tools/harness/fake_natpmp.py --mode ok        # opens the port
    python3 tools/harness/fake_natpmp.py --mode refuse    # "not authorized"
    python3 tools/harness/fake_natpmp.py --mode otherport # opens a different one
    python3 tools/harness/fake_natpmp.py --mode private   # WAN is 10.x: double NAT
    python3 tools/harness/fake_natpmp.py --mode deaf      # listens, says nothing

Binds 127.0.0.1:5351, so the client under test is pointed at it with
XRADIO_UPNP_GATEWAY=127.0.0.1.
"""

import argparse
import socket
import struct
import sys

WAN_PUBLIC = "81.90.144.12"
WAN_PRIVATE = "10.64.0.7"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="ok",
                    choices=["ok", "refuse", "otherport", "private", "deaf"])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5351)
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.host, args.port))
    print(f"fake NAT-PMP router on {args.host}:{args.port}, mode {args.mode}",
          flush=True)

    mapped = {}
    epoch = 1
    while True:
        data, addr = s.recvfrom(1024)
        if args.mode == "deaf" or len(data) < 2:
            continue
        version, op = data[0], data[1]
        if version != 0:
            # "unsupported version", which is what a PCP-only router says
            s.sendto(struct.pack("!BBHI", 0, 128, 1, epoch), addr)
            continue

        if op == 0:                                   # what is your WAN address?
            wan = WAN_PRIVATE if args.mode == "private" else WAN_PUBLIC
            body = struct.pack("!BBHI", 0, 128, 0, epoch) + socket.inet_aton(wan)
            s.sendto(body, addr)
            print(f"  asked for the WAN address -> {wan}", flush=True)
            continue

        if op in (1, 2) and len(data) >= 12:          # map a port
            internal, suggested, lifetime = struct.unpack("!HHI", data[4:12])
            if lifetime == 0:                         # a release
                mapped.pop(internal, None)
                s.sendto(struct.pack("!BBHIHHI", 0, 128 + op, 0, epoch,
                                     internal, 0, 0), addr)
                print(f"  released {internal}", flush=True)
                continue
            if args.mode == "refuse":
                s.sendto(struct.pack("!BBHIHHI", 0, 128 + op, 2, epoch,
                                     internal, 0, 0), addr)
                print(f"  refused {internal}", flush=True)
                continue
            external = (suggested + 1) if args.mode == "otherport" else suggested
            granted = min(lifetime, 3600)
            mapped[internal] = external
            s.sendto(struct.pack("!BBHIHHI", 0, 128 + op, 0, epoch,
                                 internal, external, granted), addr)
            print(f"  mapped {internal} -> {external} for {granted}s", flush=True)
            continue

        s.sendto(struct.pack("!BBHI", 0, 128 + op, 5, epoch), addr)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
