#!/usr/bin/env python3
"""Wi-Fi captive-portal onboarding for a Wignet TV."""
from __future__ import annotations

import array
import http.server
import importlib.util
import json
import logging
from logging.handlers import RotatingFileHandler
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
from dataclasses import dataclass


LOG = logging.getLogger("wignet_tv_setup")
WIFI_IFACE = "wlan0"
AP_IP = "192.168.4.1"
AP_CONNECTION = "wignet-tv-setup-ap"
WIFI_CONNECTION_PREFIX = "wignet-tv-wifi-"
PORTAL_DIR = Path(__file__).with_name("portal")
MAX_REQUEST_BYTES = 2048


# A compact 5x7 font keeps the TV instructions visible before any network
# package or graphical runtime has been installed.
FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": ("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": ("01111", "10000", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "10010", "10010", "01100"),
    "K": ("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "10101", "01010"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    " ": ("00000",) * 7,
    ".": ("00000", "00000", "00000", "00000", "00000", "00110", "00110"),
    ":": ("00000", "00110", "00110", "00000", "00110", "00110", "00000"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    "/": ("00001", "00010", "00010", "00100", "01000", "01000", "10000"),
}


@dataclass(frozen=True)
class AccessPoint:
    ssid: str
    security: str
    signal: int


def parse_access_points(raw: str) -> list[AccessPoint]:
    """Parse nmcli's terse SSID:SECURITY:SIGNAL output and de-duplicate SSIDs."""
    unique: dict[str, AccessPoint] = {}
    for line in raw.splitlines():
        fields = line.rsplit(":", 2)
        if len(fields) != 3:
            continue
        ssid, security, signal = (field.replace(r"\:", ":") for field in fields)
        if not ssid.strip():
            continue
        try:
            point = AccessPoint(ssid.strip(), security.strip(), int(signal))
        except ValueError:
            continue
        if point.ssid not in unique or point.signal > unique[point.ssid].signal:
            unique[point.ssid] = point
    return sorted(unique.values(), key=lambda point: (-point.signal, point.ssid.casefold()))


def connected() -> bool:
    result = nmcli("-t", "-f", "GENERAL.STATE,GENERAL.CONNECTION", "device", "show", WIFI_IFACE,
                   timeout=5)
    fields = dict(line.split(":", 1) for line in result.stdout.splitlines() if ":" in line)
    # State labels are localized; the numeric state is stable. Our own AP is
    # also "connected", but does not mean the Pi joined the host Wi-Fi.
    state = fields.get("GENERAL.STATE", "").split()
    connection = fields.get("GENERAL.CONNECTION", "")
    return (result.returncode == 0 and bool(state) and state[0] == "100"
            and connection not in ("", "--", AP_CONNECTION))


def saved_wifi_profile_exists() -> bool:
    result = nmcli("-t", "--escape", "yes", "-f", "NAME,TYPE", "connection", "show", timeout=5)
    if result.returncode:
        return False
    for line in result.stdout.splitlines():
        fields = line.rsplit(":", 1)
        if len(fields) == 2 and fields[0] != AP_CONNECTION and fields[1] == "802-11-wireless":
            return True
    return False


def wait_for_saved_wifi(seconds: int = 25) -> bool:
    """Give NetworkManager's saved profile time to associate before opening AP mode."""
    if not saved_wifi_profile_exists():
        return False
    start_station()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if connected():
            return True
        time.sleep(1)
    return connected()


def scan() -> list[AccessPoint]:
    result = subprocess.run(
        ["nmcli", "-t", "--escape", "yes", "-f", "SSID,SECURITY,SIGNAL", "device", "wifi", "list",
         "ifname", WIFI_IFACE, "--rescan", "yes"],
        check=False, capture_output=True, text=True, timeout=25,
    )
    if result.returncode:
        LOG.warning("Wi-Fi scan failed: %s", result.stderr.strip())
        return []
    return parse_access_points(result.stdout)


def provisioning_ap_name() -> str:
    mac = (Path("/sys/class/net") / WIFI_IFACE / "address").read_text().strip()
    return "Wignet-" + mac.replace(":", "")[-4:].upper()


def nmcli(*args: str, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run(["nmcli", *args], check=False, capture_output=True,
                          text=True, timeout=timeout)


def start_hotspot(ap_name: str) -> None:
    """Create the open, temporary setup AP and its private DHCP range."""
    result = nmcli("radio", "wifi", "on", timeout=5)
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout).strip() or "Wi-Fi radio unavailable")
    current = nmcli("connection", "show", "id", AP_CONNECTION, timeout=5)
    if current.returncode:
        result = nmcli("connection", "add", "type", "wifi", "ifname", WIFI_IFACE,
                       "con-name", AP_CONNECTION, "ssid", ap_name)
        if result.returncode:
            raise RuntimeError((result.stderr or result.stdout).strip() or "AP setup failed")
    security = nmcli("-g", "802-11-wireless-security.key-mgmt", "connection", "show",
                     "id", AP_CONNECTION, timeout=5)
    if security.returncode:
        raise RuntimeError((security.stderr or security.stdout).strip() or "AP security lookup failed")
    if security.stdout.strip():
        # An open network has no security setting. In particular, do not leave
        # the old key-mgmt=none setting (static WEP) on an existing setup AP.
        result = nmcli("connection", "modify", AP_CONNECTION, "remove", "802-11-wireless-security")
        if result.returncode:
            raise RuntimeError((result.stderr or result.stdout).strip() or "AP security reset failed")
    result = nmcli("connection", "modify", AP_CONNECTION,
                   "connection.autoconnect", "no",
                   "802-11-wireless.mode", "ap",
                   "802-11-wireless.ssid", ap_name,
                   "802-11-wireless.band", "bg",
                   "ipv4.method", "shared",
                   "ipv4.addresses", f"{AP_IP}/24",
                   "ipv6.method", "disabled")
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout).strip() or "AP setup failed")
    result = nmcli("--wait", "25", "connection", "up", "id", AP_CONNECTION,
                   "ifname", WIFI_IFACE, timeout=35)
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout).strip() or "AP activation failed")


def runtime_ready() -> bool:
    # Look the modules up without importing them: importing Pillow and numpy
    # takes 4 s or more on a Pi Zero, at every boot.
    has_modules = all(importlib.util.find_spec(name) for name in ("PIL", "numpy"))
    return has_modules and all(shutil.which(tool) for tool in ("cec-ctl", "aplay"))


def install_runtime() -> tuple[bool, str]:
    """Install the graphical reader only after Wi-Fi has been configured."""
    update = subprocess.run(["apt-get", "update"], check=False, capture_output=True, text=True, timeout=180)
    if update.returncode:
        return False, (update.stderr or update.stdout).strip() or "Mise à jour des paquets impossible"
    install = subprocess.run(["apt-get", "install", "-y", "--no-install-recommends", "python3-pil", "python3-numpy",
                              "v4l-utils", "alsa-utils"],
                             check=False, capture_output=True, text=True, timeout=300)
    return install.returncode == 0, (install.stderr or install.stdout).strip()


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)


class SetupScreen:
    """Draw readable setup instructions directly to the Pi's HDMI framebuffer."""
    def __init__(self):
        fb = Path("/sys/class/graphics/fb0")
        self.width, self.height = map(int, (fb / "virtual_size").read_text().strip().split(","))
        self.bpp = int((fb / "bits_per_pixel").read_text().strip())
        self.stride = int((fb / "stride").read_text().strip())
        if self.bpp != 16 or self.stride < self.width * 2 or self.width * self.height > 4_000_000:
            raise RuntimeError(f"unsupported framebuffer {self.width}x{self.height} {self.bpp}bpp")
        self.fd = os.open("/dev/fb0", os.O_WRONLY)
        self.previous = None

    def _text(self, frame: array.array, text: str, y: int, scale: int, color: int):
        text = text.upper()
        char_width = 6 * scale
        x = max(0, (self.width - len(text) * char_width) // 2)
        on = array.array("H", [color]) * scale
        for char in text:
            glyph = FONT.get(char, FONT[" "] if " " in FONT else ("00000",) * 7)
            for gy, row in enumerate(glyph):
                for gx, bit in enumerate(row):
                    if bit != "1":
                        continue
                    px = x + gx * scale
                    for dy in range(scale):
                        offset = (y + gy * scale + dy) * self.width + px
                        frame[offset:offset + scale] = on
            x += char_width

    def show(self, ap_name: str, state: str = "ready", detail: str = ""):
        signature = (ap_name, state, detail)
        if signature == self.previous:
            return
        self.previous = signature
        width, height = self.width, self.height
        paper = rgb565(247, 242, 232)
        ink = rgb565(51, 32, 33)
        muted = rgb565(107, 89, 84)
        coral = rgb565(186, 74, 79)
        frame = array.array("H", [paper]) * (width * height)
        accent = array.array("H", [coral]) * width
        frame[:width * 12] = accent * 12
        scale = max(2, min(8, width // 150))
        self._text(frame, "WIGNET", height // 7, scale, coral)
        if state == "preparing":
            self._text(frame, "WIFI CONNECTE", height * 2 // 7, max(2, scale - 1), ink)
            self._text(frame, "PREPARATION DE L AFFICHAGE", height * 3 // 7,
                       max(2, scale // 2), muted)
            self._text(frame, "LE PREMIER ECRAN ARRIVE", height * 4 // 7,
                       max(2, scale // 2), coral)
        else:
            title = "CONNEXION AU RESEAU" if state == "connecting" else (
                "CONNEXION IMPOSSIBLE" if state == "error" else "CONFIGURER LE WIFI"
            )
            self._text(frame, title, height * 2 // 7, max(2, scale - 1), ink)
            if state == "connecting":
                lines = ["WIGNET ESSAIE DE REJOINDRE", detail]
            elif state == "error":
                lines = ["REESSAYEZ DEPUIS LE PORTAIL", detail]
            else:
                lines = ["1. CONNECTEZ VOTRE TELEPHONE AU RESEAU"]
            y = height * 3 // 7
            for line in lines:
                self._text(frame, line, y, max(2, scale // 2), muted)
                y += height // 12
            self._text(frame, ap_name, height * 4 // 7, scale, coral)
            self._text(frame, "2. LE PORTAIL WIFI S OUVRIRA AUTOMATIQUEMENT", height * 5 // 7,
                       max(2, scale // 2), ink)
            self._text(frame, f"SINON OUVREZ HTTP {AP_IP}", height * 6 // 7,
                       max(2, scale // 2), muted)
        row_bytes = width * 2
        if self.stride == row_bytes:
            data = frame.tobytes()
        else:
            blank = array.array("H", [0]) * ((self.stride - row_bytes) // 2)
            padded = array.array("H")
            for row in range(height):
                start = row * width
                padded.extend(frame[start:start + width])
                padded.extend(blank)
            data = padded.tobytes()
        if sys.byteorder != "little":
            raise RuntimeError("RGB565 setup screen requires a little-endian CPU")
        written = os.pwrite(self.fd, data, 0)
        if written != len(data):
            raise OSError(f"short framebuffer write: {written} of {len(data)}")


class PortalState:
    def __init__(self, ap_name: str):
        self.ap_name = ap_name
        self.status = "ready"
        self.ssid = ""
        self.lock = threading.Lock()
        self.connected = threading.Event()
        self.cached_networks: list[AccessPoint] = []
        self.scanned_at = 0.0

    def networks(self, force: bool = False):
        with self.lock:
            due = time.monotonic() - self.scanned_at > 30
            if force or due:
                self.cached_networks = scan()
                self.scanned_at = time.monotonic()
            return list(self.cached_networks)


def join_network(ssid: str, password: str) -> None:
    """Save credentials in NetworkManager, then switch the radio to the Wi-Fi."""
    connection = WIFI_CONNECTION_PREFIX + uuid.uuid4().hex[:12]
    try:
        result = nmcli("connection", "add", "type", "wifi", "ifname", WIFI_IFACE,
                       "con-name", connection, "ssid", ssid)
        if result.returncode:
            raise RuntimeError("profile")
        result = nmcli("connection", "modify", connection, "connection.autoconnect", "yes")
        if result.returncode:
            raise RuntimeError("profile")
        if password:
            result = nmcli("connection", "modify", connection,
                           "802-11-wireless-security.key-mgmt", "wpa-psk",
                           "connection.autoconnect", "yes")
            if result.returncode:
                raise RuntimeError("security")
            with tempfile.NamedTemporaryFile(mode="w", prefix="wignet-tv-pass-", delete=False) as secret:
                os.chmod(secret.name, 0o600)
                secret.write("802-11-wireless-security.psk:" + password + "\n")
                secret_path = secret.name
            try:
                result = nmcli("--wait", "45", "connection", "up", "id", connection,
                               "ifname", WIFI_IFACE, "passwd-file", secret_path, timeout=55)
            finally:
                os.unlink(secret_path)
        else:
            result = nmcli("--wait", "45", "connection", "up", "id", connection,
                           "ifname", WIFI_IFACE, timeout=55)
        if result.returncode or not connected():
            raise RuntimeError("connect")
    except Exception:
        nmcli("connection", "delete", "id", connection, timeout=10)
        raise


def start_station() -> None:
    """NetworkManager joins a saved network by itself."""


# The Buildroot image has no NetworkManager: the same operations on
# wpa_supplicant, udhcpc, hostapd and dnsmasq.
if os.environ.get("WIGNET_WIFI_BACKEND") == "wpa":
    import wifi_wpa
    connected = wifi_wpa.connected
    saved_wifi_profile_exists = wifi_wpa.saved_wifi_profile_exists
    start_hotspot = wifi_wpa.start_hotspot
    start_station = wifi_wpa.start_station
    join_network = wifi_wpa.join_network

    def scan() -> list[AccessPoint]:
        return parse_access_points(wifi_wpa.scan_raw())


def connect_network(state: PortalState, ssid: str, password: str):
    """Join the submitted network, or reopen the setup AP if that fails."""
    try:
        join_network(ssid, password)
        with state.lock:
            state.status = "connected"
        state.connected.set()
        LOG.info("Connected to Wi-Fi network %s", ssid)
    except Exception as exc:
        LOG.warning("Wi-Fi setup failed for %s: %s", ssid, exc)
        try:
            start_hotspot(state.ap_name)
        except Exception as ap_error:
            LOG.error("Could not restore setup hotspot: %s", ap_error)
        with state.lock:
            state.status = "error"


def portal_handler(state: PortalState):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = "WignetSetup/1"

        def log_message(self, _format, *_args):
            # In particular, never log query strings or submitted credentials.
            LOG.info("portal %s %s", self.command, urlsplit(self.path).path)

        def send_bytes(self, status: int, data: bytes, content_type: str):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)

        def redirect(self):
            self.send_response(302)
            self.send_header("Location", f"http://{AP_IP}/wifi")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            path = urlsplit(self.path).path
            if path in ("/", "/wifi") or path.startswith("/hotspot-detect") or path in (
                "/generate_204", "/gen_204", "/connecttest.txt", "/ncsi.txt"
            ):
                if path.startswith("/hotspot-detect") or path in (
                    "/generate_204", "/gen_204", "/connecttest.txt", "/ncsi.txt"
                ):
                    self.redirect()
                    return
                filename = "wifi.html"
                self.send_bytes(200, (PORTAL_DIR / filename).read_bytes(), "text/html; charset=utf-8")
            elif path == "/connect":
                self.send_bytes(200, (PORTAL_DIR / "connect.html").read_bytes(), "text/html; charset=utf-8")
            elif path == "/device.json":
                data = json.dumps({"ap": state.ap_name}).encode()
                self.send_bytes(200, data, "application/json; charset=utf-8")
            elif path == "/networks.json":
                force = "refresh=1" in urlsplit(self.path).query
                networks = state.networks(force)
                data = json.dumps({"networks": [
                    {"ssid": point.ssid, "rssi": point.signal // 2 - 100,
                     "secure": bool(point.security)} for point in networks
                ]}, ensure_ascii=False).encode()
                self.send_bytes(200, data, "application/json; charset=utf-8")
            elif path.startswith("/static/"):
                filename = Path(path).name
                content_types = {"portal.css": "text/css", "wignet-portal.css": "text/css",
                                 "portal.js": "application/javascript"}
                if filename not in content_types:
                    self.send_error(404)
                    return
                self.send_bytes(200, (PORTAL_DIR / "static" / filename).read_bytes(),
                                content_types[filename] + "; charset=utf-8")
            else:
                self.redirect()

        def do_POST(self):
            if urlsplit(self.path).path != "/wifisave":
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self.send_error(400)
                return
            if length <= 0 or length > MAX_REQUEST_BYTES:
                self.send_error(413 if length > MAX_REQUEST_BYTES else 400)
                return
            fields = parse_qs(self.rfile.read(length).decode("utf-8", "strict"), keep_blank_values=True)
            ssid = fields.get("s", [""])[0]
            password = fields.get("p", [""])[0]
            if (not ssid or len(ssid.encode("utf-8")) > 32 or "\n" in ssid or "\r" in ssid
                    or "\n" in password or "\r" in password or len(password) > 64):
                self.send_error(400, "Invalid Wi-Fi credentials")
                return
            with state.lock:
                if state.status == "connecting":
                    self.send_error(409, "Connection already in progress")
                    return
                state.status = "connecting"
                state.ssid = ssid
            data = b"{\"status\":\"connecting\"}"
            self.send_bytes(200, data, "application/json; charset=utf-8")
            threading.Thread(target=connect_network, args=(state, ssid, password), daemon=True).start()

    return Handler


class PortalServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def prepare_connected_runtime() -> int:
    """Install the reader once Wi-Fi is configured."""
    if runtime_ready():
        return 0
    success, detail = install_runtime()
    if not success:
        LOG.error("Wignet TV runtime installation failed: %s", detail)
        return 1
    return 0


def run_setup() -> int:
    screen = None
    server = None
    try:
        if connected() or wait_for_saved_wifi():
            return prepare_connected_runtime()
        ap_name = provisioning_ap_name()
        state = PortalState(ap_name)
        try:
            screen = SetupScreen()
        except (OSError, ValueError, RuntimeError) as exc:
            LOG.warning("TV setup screen unavailable: %s", exc)
        server = PortalServer(("0.0.0.0", 80), portal_handler(state))
        server.timeout = 0.5
        start_hotspot(ap_name)
        LOG.info("Setup portal ready at http://%s for network %s", AP_IP, ap_name)
        while not state.connected.is_set():
            with state.lock:
                status, ssid = state.status, state.ssid
            if screen:
                screen.show(ap_name, status, ssid)
            server.handle_request()
        if not runtime_ready() and screen:
            screen.show(ap_name, "preparing")
        return prepare_connected_runtime()
    except Exception as exc:
        LOG.exception("Wignet TV Wi-Fi setup failed: %s", exc)
        if screen:
            try:
                screen.show(ap_name, "error", "NOUVEL ESSAI DANS 5 SECONDES")
            except Exception:
                LOG.exception("Could not display Wi-Fi setup error")
        return 1
    finally:
        if server:
            server.server_close()
        if screen:
            os.close(screen.fd)


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    log_path = os.environ.get("WIGNET_SETUP_LOG")
    if log_path:
        try:
            handler = RotatingFileHandler(log_path, maxBytes=1_000_000, backupCount=2)
            handler.setFormatter(logging.Formatter("%(asctime)s %(name)s %(levelname)s %(message)s"))
            LOG.addHandler(handler)
        except OSError:
            LOG.exception("Could not open persistent Wi-Fi setup log")
    while run_setup() != 0:
        LOG.warning("Retrying Wi-Fi setup in 5 seconds")
        time.sleep(5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
