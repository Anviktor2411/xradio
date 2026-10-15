#!/usr/bin/env python3
"""A stand-in for SimBrief's xml.fetcher.php, for tools/check_simbrief.sh.

Answers GET /api/xml.fetcher.php?username=tester&json=1 (or userid=12345) with the fixture OFP, anything
else with HTTP 400 and SimBrief's own error shape -- exactly what the real endpoint does for an unknown
user or a pilot who has not planned a flight yet.
"""
import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse


class Handler(BaseHTTPRequestHandler):
    fixture = b"{}"
    users = {"tester", "12345"}

    def do_GET(self):  # noqa: N802
        u = urlparse(self.path)
        q = parse_qs(u.query)
        who = (q.get("username") or q.get("userid") or [""])[0]
        if u.path == "/api/xml.fetcher.php" and who in self.users:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(self.fixture)))
            self.end_headers()
            self.wfile.write(self.fixture)
            return
        body = json.dumps({"fetch": {"status": "Error: Unknown UserID", "time": "0"}}).encode()
        self.send_response(400)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stdout.write("fake_simbrief: " + (fmt % args) + "\n")
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=48790)
    ap.add_argument("--fixture", default="tools/fixtures/simbrief_ofp.json")
    a = ap.parse_args()
    with open(a.fixture, "rb") as f:
        Handler.fixture = f.read()
    srv = HTTPServer(("127.0.0.1", a.port), Handler)
    print(f"fake_simbrief: listening on 127.0.0.1:{a.port}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
