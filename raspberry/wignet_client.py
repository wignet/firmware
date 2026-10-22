#!/usr/bin/python3
# -*- coding:utf-8 -*-
"""Wignet backend client for a Raspberry Pi driving a Waveshare 7.5" V2 e-paper.

Mirrors the ESP32 firmware's poll loop (esp32/src/main.cpp):
fetch the rendered BMP for this device's MAC + display size, cache it via
ETag/If-None-Match, then ask the server how long to wait before polling
again. Unlike the ESP32 there is no deep sleep here - the Pi stays powered,
so the loop just sleeps between polls (equivalent to the firmware's
"active watch" mode).
"""
import io
import logging
import os
import sys
import time

import requests
from PIL import Image

clientdir = os.path.dirname(os.path.realpath(__file__))
libdir = os.path.join(clientdir, "lib")
if os.path.exists(libdir):
    sys.path.append(libdir)

from waveshare_epd import epd7in5_V2

WIGNET_HOST = "api.wignet.app"
WIGNET_PORT = 80
WIGNET_IMAGE_PATH = "/pic/4displayB5.bmp"
WIGNET_SCHEDULE_PATH = "/schedule"
DEFAULT_REFRESH_SEC = 30
REQUEST_TIMEOUT_SEC = 30

ETAG_CACHE_PATH = os.path.join(clientdir, ".wignet_etag")

logging.basicConfig(level=logging.INFO)
log = logging.getLogger("wignet_client")


def get_mac_address():
    for iface in ("wlan0", "eth0"):
        path = f"/sys/class/net/{iface}/address"
        if os.path.exists(path):
            with open(path) as f:
                return f.read().strip().upper()
    raise RuntimeError("No wlan0/eth0 interface found for MAC address")


def load_cached_etag():
    try:
        with open(ETAG_CACHE_PATH) as f:
            return f.read().strip()
    except FileNotFoundError:
        return ""


def save_cached_etag(etag):
    with open(ETAG_CACHE_PATH, "w") as f:
        f.write(etag)


def fetch_schedule(mac, width, height):
    url = f"http://{WIGNET_HOST}:{WIGNET_PORT}{WIGNET_SCHEDULE_PATH}"
    params = {"mac": mac, "w": width, "h": height}
    try:
        resp = requests.get(url, params=params, timeout=10)
        resp.raise_for_status()
        value = resp.json().get("nextRefreshSec")
        if isinstance(value, int) and value > 0:
            log.info("[Schedule] Next refresh in %ds", value)
            return value
        log.warning("[Schedule] Missing/invalid nextRefreshSec, using default")
    except Exception as e:
        log.warning("[Schedule] Failed: %s", e)
    return DEFAULT_REFRESH_SEC


def fetch_and_display(epd, mac, width, height):
    url = f"http://{WIGNET_HOST}:{WIGNET_PORT}{WIGNET_IMAGE_PATH}"
    params = {"mac": mac, "w": width, "h": height}
    headers = {}
    cached_etag = load_cached_etag()
    if cached_etag:
        headers["If-None-Match"] = cached_etag

    resp = requests.get(url, params=params, headers=headers,
                         timeout=REQUEST_TIMEOUT_SEC)

    if resp.status_code == 304:
        log.info("Image unchanged (304), skipping redraw")
        return

    resp.raise_for_status()

    image = Image.open(io.BytesIO(resp.content)).convert("1")
    epd.init()
    epd.display(epd.getbuffer(image))
    epd.sleep()
    log.info("Display updated (%dx%d)", *image.size)

    new_etag = resp.headers.get("ETag")
    if new_etag:
        save_cached_etag(new_etag)
        log.info("[ETag] Cached: %s", new_etag)


def main():
    mac = get_mac_address()
    log.info("MAC: %s", mac)

    epd = epd7in5_V2.EPD()
    width, height = epd.width, epd.height
    log.info("Display: %dx%d", width, height)

    while True:
        next_refresh = DEFAULT_REFRESH_SEC
        try:
            fetch_and_display(epd, mac, width, height)
            next_refresh = fetch_schedule(mac, width, height)
        except Exception as e:
            log.error("Fetch cycle failed: %s", e)

        log.info("Sleeping %ds until next poll", next_refresh)
        time.sleep(next_refresh)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log.info("Interrupted, exiting")
        epd7in5_V2.epdconfig.module_exit()
