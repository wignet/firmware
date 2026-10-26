#!/usr/bin/env python3
"""Install an experimental application through USB, or authorize its HTTPS download.

The first installation/partition migration still uses PlatformIO's USB uploader.
This script requires its ota.json plus adjacent firmware-ota.bin, and an explicit
serial port. It never reads or resets Wi-Fi credentials. Requires pyserial.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import time
from urllib.parse import urlsplit
from ota_package import TARGET, LAYOUT, validate_image


def load_artifact(manifest_path):
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get("schema") != "wignet-ota-bench-v1" or
            manifest.get("target") != TARGET or manifest.get("layout") != LAYOUT or
            manifest.get("image") != "firmware-ota.bin"):
        raise ValueError("incompatible OTA bench manifest")
    data = (manifest_path.parent / "firmware-ota.bin").read_bytes()
    validate_image(data)
    if manifest.get("size") != len(data) or manifest.get("sha256") != hashlib.sha256(data).hexdigest():
        raise ValueError("application does not match manifest size/SHA-256")
    return manifest, data


def wait(port, prefix, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        print(line)
        if line.startswith("[bench] error"):
            raise RuntimeError(line)
        if line.startswith(prefix):
            return line
    raise TimeoutError(f"no {prefix!r} within {timeout} seconds")


def hold(port):
    # Opening a UART bridge can reset the ESP32 despite idle DTR/RTS.
    # A command sent during reset is lost; retry while setup/redraw completes.
    for _ in range(30):
        port.write(b"ota hold\n")
        try:
            return wait(port, "[bench] OTA slot=", 3)
        except TimeoutError:
            pass
    raise TimeoutError("OTA console did not answer within 90 seconds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--port", required=True)
    parser.add_argument("--url", help="direct HTTPS URL of this exact application (no redirects)")
    parser.add_argument("--check", action="store_true", help="validate local files only; no serial access")
    args = parser.parse_args()
    manifest, data = load_artifact(args.manifest)
    if args.url:
        url = urlsplit(args.url)
        if url.scheme != "https" or not url.hostname or url.username or url.password or any(c.isspace() for c in args.url):
            parser.error("--url must be direct HTTPS without credentials or whitespace")
    print(f"{manifest['environment']}: {len(data)} bytes, SHA-256 {manifest['sha256']}")
    if manifest.get("fault_injection"):
        print("FAULT INJECTION: this application must fail its trial and roll back.")
    if args.check:
        return
    import serial
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 0.2
    port.write_timeout = 10
    port.dtr = port.rts = False
    port.open()
    try:
        initial = hold(port)
        if "trial=0" not in initial or "layout=ota-4m-v1" not in initial:
            raise RuntimeError("install requires a confirmed application with the OTA layout")
        initial_slot = initial.split("slot=", 1)[1].split()[0]
        if args.url:
            port.write(f"ota {len(data)} {manifest['sha256']} {args.url}\n".encode("ascii"))
        else:
            port.write(f"ota-image {len(data)} {manifest['sha256']}\n".encode("ascii"))
            wait(port, "[bench] OTA ready:", 30)
            for at in range(0, len(data), 4096):
                chunk = data[at:at + 4096]
                if port.write(chunk) != len(chunk):
                    raise RuntimeError("short serial write")
                if at + 4096 < len(data):
                    wait(port, "[bench] +", 60)
        wait(port, "[bench] OTA staged", 330)
        print("Application staged; rebooting. Watching trial outcome...")
        expected = "[OTA] Confirmed" if not manifest.get("fault_injection") else "[OTA] Trial failed"
        wait(port, expected, 210)
        if manifest.get("fault_injection"):
            # A rejection log is not evidence the previous app booted.
            recovered = wait(port, "[OTA] layout=ota-4m-v1", 30)
            if f"slot={initial_slot} trial=0" not in recovered:
                raise RuntimeError("did not observe the previous confirmed slot after rejection")
        print("Trial outcome observed; check the panel and normal poll/sleep cycle too.")
    finally:
        port.close()


if __name__ == "__main__":
    main()
