#!/usr/bin/env python3
"""Mock the barkcam config UI for local preview / README screenshots.

Serves the real page from include/ui_page.h with fake /config and an
animated /level (quiet room + two bark bursts), so you can see the UI in a
browser without hardware:

    python3 tools/mock_ui.py            # http://127.0.0.1:8653
"""
import json
import random
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler

ROOT = __file__.rsplit("/", 2)[0]
src = open(f"{ROOT}/include/ui_page.h").read()
UI_PAGE = src[src.index('R"HTML(') + 7 : src.rindex(')HTML"')]

HIST_LEN = 40
random.seed(7)


def level_payload():
    """Quiet ambient noise with two bark-shaped bursts (fast attack, decay)."""
    hist = []
    for i in range(HIST_LEN):
        v = random.uniform(0.08, 0.2)          # ambient room noise
        for center in (10, 28):                # two barks in the window
            d = i - center
            if 0 <= d < 7:                     # ~50-110 ms burst
                v = max(v, 0.85 - d * 0.13)
        hist.append(round(v, 3))
    noise = round(random.uniform(0.12, 0.18), 3)
    thr = round(min(1.0, noise + 15 / 60), 3)  # margin 15 dB over -80..-20 span
    return {"mode": "config + online", "db": hist[-1], "noise": noise,
            "thr": thr, "hist": hist}


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            self._send(200, "text/html", UI_PAGE.encode())
        elif self.path == "/config":
            self._send(200, "application/json", json.dumps({
                "ssid": "MyHomeWiFi", "pass": "",
                "token": "123456789:AAFAKE-TOKEN-VALUE-FOR-SCREENSHOT",
                "chatId": "123456789",
                "margin": 15, "rotate": 3, "exposure": 1,
                # demo state for screenshots: all days on, 11pm–6am quiet
                "daysMask": 127, "hoursMask": 8388480,
            }).encode())
        elif self.path == "/level":
            self._send(200, "application/json", json.dumps(level_payload()).encode())
        else:
            self._send(404, "text/plain", b"not found")

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8653
    print(f"mock barkcam UI on http://127.0.0.1:{port}")
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
