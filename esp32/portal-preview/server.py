#!/usr/bin/env python3
"""Local preview of the Wignet Wi-Fi setup portal.

Serves the files the device embeds (../portal/, via embed_portal.py) on the same
routes, and stands in for the device's JSON endpoints and WiFiManager's
POST /wifisave with sample data.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ESP32_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ESP32_DIR))
from embed_portal import ASSETS, PORTAL  # noqa: E402 - routes shared with the firmware

AP_NAME = "Wignet-0002"
NETWORKS = (
    {"ssid": "Reseau-Demo", "rssi": -54, "secure": True},
    {"ssid": "Livebox-2A10", "rssi": -63, "secure": True},
    {"ssid": "atelier", "rssi": -74, "secure": True},
    {"ssid": "FreeWifi", "rssi": -86, "secure": False},
)
ROUTES = {route: (PORTAL / name, content_type) for route, name, content_type in ASSETS}


class PreviewHandler(BaseHTTPRequestHandler):
    server_version = "WignetPortalPreview/2.0"
    no_networks = False

    def do_GET(self) -> None:  # noqa: N802 - stdlib API
        parsed = urlparse(self.path)
        if parsed.path in ROUTES:
            path, content_type = ROUTES[parsed.path]
            self.send_body(path.read_bytes(), content_type)
        elif parsed.path == "/networks.json":
            if "refresh" in parse_qs(parsed.query):
                time.sleep(1.2)  # a real scan takes a couple of seconds
            networks = [] if self.no_networks else list(NETWORKS)
            self.send_json({"networks": networks})
        elif parsed.path == "/device.json":
            self.send_json({"ap": AP_NAME})
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:  # noqa: N802 - stdlib API
        if urlparse(self.path).path != "/wifisave":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        content_length = int(self.headers.get("Content-Length", "0"))
        values = parse_qs(self.rfile.read(content_length).decode("utf-8"))
        ssid = values.get("s", [""])[0].strip()
        if ssid == "fail":  # exercise the portal's retry path
            self.send_error(HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        # WiFiManager answers with its own page; the portal only checks for 200.
        # Credentials are not stored.
        self.send_body(b"<!doctype html><title>Credentials saved</title>", "text/html; charset=utf-8")

    def send_body(self, body: bytes, content_type: str) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value: object) -> None:
        self.send_body(json.dumps(value, ensure_ascii=False).encode("utf-8"), "application/json")

    def log_message(self, format: str, *args: object) -> None:
        print(f"[portal-preview] {self.address_string()} {format % args}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4173)
    parser.add_argument("--no-networks", action="store_true", help="answer every scan with an empty list")
    args = parser.parse_args()
    PreviewHandler.no_networks = args.no_networks
    server = ThreadingHTTPServer((args.host, args.port), PreviewHandler)
    print(f"Wignet portal preview: http://{args.host}:{args.port}")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping preview.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
