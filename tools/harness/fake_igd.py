#!/usr/bin/env python3
"""A fake home router, for testing the UPnP client without a real one.

Speaks just enough SSDP and SOAP to stand in for an Internet Gateway Device,
and deliberately does the awkward things real firmware does: the description
comes back chunked, its control URL is relative and goes through <URLBase>,
and in `lease` mode the router refuses a permanent mapping with error 725
until asked for a timed one.

    python3 tools/harness/fake_igd.py --mode ok|lease|refuse|private-wan|deaf

Then run the client with XRADIO_UPNP_GATEWAY=127.0.0.1 so its unicast
M-SEARCH lands here. Exits when its parent tells it to (SIGTERM).
"""

import argparse
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

HTTP_PORT = 5000
SSDP_PORT = 1900
SERVICE = "urn:schemas-upnp-org:service:WANIPConnection:1"

state = {"mode": "ok", "mappings": {}, "requests": []}

DESC = f"""<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <URLBase>http://127.0.0.1:{HTTP_PORT}/</URLBase>
  <device>
    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
    <friendlyName>Fake Router</friendlyName>
    <deviceList><device>
      <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
      <deviceList><device>
        <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
        <serviceList><service>
          <serviceType>{SERVICE}</serviceType>
          <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
          <controlURL>ctl/IPConn</controlURL>
          <eventSubURL>evt/IPConn</eventSubURL>
          <SCPDURL>WANIPCn.xml</SCPDURL>
        </service></serviceList>
      </device></deviceList>
    </device></deviceList>
  </device>
</root>
"""


def soap_ok(action, inner=""):
    return (f'<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            f'<s:Body><u:{action}Response xmlns:u="{SERVICE}">{inner}</u:{action}Response>'
            f'</s:Body></s:Envelope>')


def soap_fault(code, desc):
    return (f'<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            f'<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>'
            f'<detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0">'
            f'<errorCode>{code}</errorCode><errorDescription>{desc}</errorDescription>'
            f'</UPnPError></detail></s:Fault></s:Body></s:Envelope>')


def tag(body, name):
    a = body.find(f"<{name}>")
    b = body.find(f"</{name}>")
    return body[a + len(name) + 2:b] if a >= 0 and b > a else ""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        state["requests"].append(("GET", self.path))
        if self.path == "/desc/root.xml":
            body = DESC.encode()
            # Chunked on purpose: some routers do this and a naive client
            # ends up with hex chunk sizes inside its XML.
            self.send_response(200)
            self.send_header("Content-Type", "text/xml")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 700):
                chunk = body[i:i + 700]
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_error(404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        action = self.headers.get("SOAPAction", "").split("#")[-1].strip('"')
        state["requests"].append(("POST", self.path, action))
        if self.path != "/ctl/IPConn":
            self.send_error(404)
            return

        mode = state["mode"]
        status, resp = 200, ""
        if action == "GetExternalIPAddress":
            ip = "10.44.0.9" if mode == "private-wan" else "203.0.113.7"
            resp = soap_ok(action, f"<NewExternalIPAddress>{ip}</NewExternalIPAddress>")
        elif action == "AddPortMapping":
            port = tag(body, "NewExternalPort")
            lease = tag(body, "NewLeaseDuration")
            if mode == "refuse":
                status, resp = 500, soap_fault(718, "ConflictInMappingEntry")
            elif mode == "lease" and lease == "0":
                status, resp = 500, soap_fault(725, "OnlyPermanentLeasesSupported")
            else:
                state["mappings"][port] = {"client": tag(body, "NewInternalClient"),
                                           "proto": tag(body, "NewProtocol"), "lease": lease}
                resp = soap_ok(action)
        elif action == "DeletePortMapping":
            port = tag(body, "NewExternalPort")
            if port in state["mappings"]:
                del state["mappings"][port]
                resp = soap_ok(action)
            else:
                status, resp = 500, soap_fault(714, "NoSuchEntryInArray")
        else:
            status, resp = 500, soap_fault(401, "Invalid Action")

        data = resp.encode()
        self.send_response(status)
        self.send_header("Content-Type", 'text/xml; charset="utf-8"')
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def ssdp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", SSDP_PORT))
    while True:
        data, addr = s.recvfrom(2048)
        if not data.startswith(b"M-SEARCH"):
            continue
        state["requests"].append(("MSEARCH", addr[0]))
        reply = ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nEXT:\r\n"
                 f"LOCATION: http://127.0.0.1:{HTTP_PORT}/desc/root.xml\r\n"
                 "SERVER: FakeOS/1.0 UPnP/1.0 fake_igd/1\r\n"
                 "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                 "USN: uuid:fake-igd::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n")
        s.sendto(reply.encode(), addr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="ok",
                    choices=["ok", "lease", "refuse", "private-wan", "deaf"])
    args = ap.parse_args()
    state["mode"] = args.mode
    if args.mode == "deaf":
        print("deaf: not answering anything", flush=True)
        threading.Event().wait()
        return
    threading.Thread(target=ssdp, daemon=True).start()
    srv = HTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    print(f"fake router up: mode={args.mode}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
