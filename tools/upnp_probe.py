#!/usr/bin/env python3
"""Ask the router, the way XRadio asks it, and print what it says.

When the Hosting tab reports "no router answered", there are two very
different explanations: the router has UPnP switched off, or XRadio asked on
the wrong network interface. This tells you which, without the sim.

    python3 tools/upnp_probe.py [router address]

The address is optional; without it the default gateway is used. The same
XRADIO_UPNP_GATEWAY environment variable the plugin honours works here too.

It sends the same five M-SEARCH queries XRadio sends -- to the multicast
group and straight at the default gateway -- and prints every reply.
"""

import re
import socket
import struct
import subprocess
import sys
import time

TARGETS = [
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
]


def default_gateway():
    """The router this machine sends internet traffic through."""
    try:                                     # Linux
        with open("/proc/net/route") as f:
            for line in f.readlines()[1:]:
                parts = line.split()
                if parts[1] == "00000000":
                    return socket.inet_ntoa(struct.pack("<L", int(parts[2], 16)))
    except OSError:
        pass
    try:                                     # macOS / BSD
        out = subprocess.run(["route", "-n", "get", "default"],
                             capture_output=True, text=True, timeout=5).stdout
        m = re.search(r"gateway:\s*([0-9.]+)", out)
        if m:
            return m.group(1)
    except (OSError, subprocess.SubprocessError):
        pass
    try:                                     # Windows
        out = subprocess.run(["ipconfig"], capture_output=True, text=True,
                             timeout=5).stdout
        m = re.search(r"Default Gateway[ .]*:\s*([0-9.]+)", out)
        if m:
            return m.group(1)
    except (OSError, subprocess.SubprocessError):
        pass
    return ""


def local_address(gateway):
    """The address of the interface that carries the route to the gateway."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((gateway or "8.8.8.8", 1900))
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    import os
    gw = (sys.argv[1] if len(sys.argv) > 1 else
          os.environ.get("XRADIO_UPNP_GATEWAY", "") or default_gateway())
    me = local_address(gw)
    print(f"this machine : {me}")
    print(f"gateway      : {gw or 'not found'}")
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((me, 0))
    # Multicast has to leave by the interface that reaches the router, not
    # whichever one the OS picks first -- on a machine with VirtualBox or a
    # VPN installed that is usually the wrong one.
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(me))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    s.settimeout(0.3)

    for target in TARGETS:
        req = ("M-SEARCH * HTTP/1.1\r\n"
               "HOST: 239.255.255.250:1900\r\n"
               'MAN: "ssdp:discover"\r\n'
               "MX: 1\r\n"
               f"ST: {target}\r\n\r\n").encode()
        s.sendto(req, ("239.255.255.250", 1900))
        if gw:
            s.sendto(req, (gw, 1900))

    seen = {}
    end = time.time() + 4.0
    while time.time() < end:
        try:
            data, addr = s.recvfrom(4096)
        except socket.timeout:
            continue
        text = data.decode("utf-8", "replace")
        m = re.search(r"(?im)^LOCATION:\s*(\S+)", text)
        seen.setdefault(addr[0], set()).add(m.group(1) if m else "(no LOCATION header)")

    if not seen:
        print("Nothing answered.")
        print()
        print("That means the router is not running UPnP, or has it switched off.")
        print("It is usually called 'UPnP' or 'UPnP IGD' in the router's settings,")
        print("sometimes under NAT, Firewall or Advanced. If you cannot turn it on,")
        print("forward the UDP port by hand -- the Hosting tab prints exactly what")
        print("to forward and where.")
        return 1

    print("Answered:")
    for ip, locations in seen.items():
        print(f"  {ip}")
        for loc in sorted(locations):
            print(f"      {loc}")
    print()
    print("A router answered here, so if XRadio still says nothing did, that is")
    print("XRadio's bug, not the router's -- please report it with this output.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
