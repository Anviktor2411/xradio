#!/usr/bin/env python3
"""A stand-in for GitHub's releases endpoint, in each of its moods.

    python3 tools/harness/fake_release.py --mode newer     # a later version exists
    python3 tools/harness/fake_release.py --mode same      # you are up to date
    python3 tools/harness/fake_release.py --mode older     # the site is behind
    python3 tools/harness/fake_release.py --mode garbage   # not JSON at all
    python3 tools/harness/fake_release.py --mode error     # HTTP 500
    python3 tools/harness/fake_release.py --mode huge      # megabytes of nothing

The client under test is pointed at it with
XRADIO_UPDATE_URL=http://127.0.0.1:5399/latest.
"""

import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

MODE = "newer"

RELEASE = {
    "url": "https://api.github.com/repos/Anviktor2411/xradio/releases/1",
    "html_url": "https://github.com/Anviktor2411/xradio/releases/tag/v9.9.9",
    "author": {"login": "doesvic",
               "html_url": "https://github.com/Anviktor2411"},
    "tag_name": "v9.9.9",
    "name": "XRadio v9.9.9",
    "body": "notes",
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if MODE == "garbage":
            body = b"<html>not json at all</html>"
        elif MODE == "huge":
            body = b'{"tag_name":"v9.9.9","padding":"' + b"x" * 2_000_000 + b'"}'
        elif MODE == "error":
            self.send_response(500)
            self.end_headers()
            return
        else:
            r = dict(RELEASE)
            if MODE == "same":
                # Must match the version update_test says it is running, or
                # this mood silently turns into the "older" one. The test
                # asserts on the value, so a change here fails loudly.
                r["tag_name"] = "v0.5.2"
            elif MODE == "older":
                r["tag_name"] = "v0.1.3"
            body = json.dumps(r).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="newer",
                    choices=["newer", "same", "older", "garbage", "error", "huge"])
    ap.add_argument("--port", type=int, default=5399)
    args = ap.parse_args()
    MODE = args.mode
    # Bind first, announce second. Printing before the bind is how a caller
    # that waits for this line ends up talking to a port nothing is listening
    # on yet -- or, worse, to the previous run's server that never exited.
    server = HTTPServer(("127.0.0.1", args.port), Handler)
    print(f"fake release endpoint on {args.port}, mode {args.mode}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
