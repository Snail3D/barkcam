#!/usr/bin/env python3
"""Bark Cam OTA server — run on the Mac, serves firmware updates to the board.

The board polls http://<this-mac>:8652/version every 10 minutes and
downloads firmware.bin when the version number is newer, then reboots.

To push an update:
  1. cd barkcam && pio run                    # build
  2. cp .pio/build/seeed_xiao_esp32s3/firmware.bin ota/firmware.bin
  3. echo <new-version> > ota/version         # must be greater than the board's
  4. (re)start this server — files are read per request, so a restart
     is only needed if the port was never open

The board's OTA_HOST (include/credentials.h) must point at this Mac's LAN IP.
"""
import http.server
import os
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8652
HERE = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/version":
            try:
                body = open(os.path.join(HERE, "version")).read().strip()
            except FileNotFoundError:
                body = "0"
            self._send(body.encode(), "text/plain")
        elif self.path == "/firmware.bin":
            p = os.path.join(HERE, "firmware.bin")
            if not os.path.exists(p):
                self.send_response(404)
                self.end_headers()
                return
            data = open(p, "rb").read()
            self._send(data, "application/octet-stream")
        else:
            self.send_response(404)
            self.end_headers()

    def _send(self, data: bytes, ctype: str):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print("[ota]", self.address_string(), fmt % args)


if __name__ == "__main__":
    print(f"barkcam OTA server on 0.0.0.0:{PORT} (serving {HERE})")
    http.server.ThreadingHTTPServer(("0.0.0.0", PORT)).serve_forever()
