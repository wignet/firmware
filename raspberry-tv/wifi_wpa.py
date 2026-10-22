"""Wi-Fi for the Wignet image: wpa_supplicant, udhcpc, hostapd and dnsmasq.

wifi_setup.py drives NetworkManager on Raspberry Pi OS.  The Buildroot image
has none of it (WIGNET_WIFI_BACKEND=wpa): this module gives wifi_setup.py the
same few operations on the plain daemons, which join a saved network in a
few seconds instead of NetworkManager's twenty.

The saved network lives in /data/wifi/wpa_supplicant.conf, the image's only
writable partition.  It holds the SSID in hex and the derived PSK, never the
passphrase itself.
"""
from __future__ import annotations

import hashlib
import os
import re
import signal
import subprocess
import time
from pathlib import Path

IFACE = "wlan0"
AP_IP = "192.168.4.1"
STATE_DIR = Path(os.environ.get("WIGNET_WIFI_DIR", "/data/wifi"))
CONF = STATE_DIR / "wpa_supplicant.conf"
RUN_DIR = Path(os.environ.get("WIGNET_WIFI_RUN", "/run/wignet-wifi"))
CTRL_DIR = "/run/wpa_supplicant"
COUNTRY = os.environ.get("WIGNET_WIFI_COUNTRY", "FR")
JOIN_SECONDS = 45

# The portal asks for the networks while the radio is the access point,
# where brcmfmac may refuse to scan: keep what was seen before it started.
_last_scan = ""


def run(*args: str, timeout: int = 15) -> subprocess.CompletedProcess:
    return subprocess.run(list(args), check=False, capture_output=True, text=True,
                          timeout=timeout)


def _require(result: subprocess.CompletedProcess, what: str) -> None:
    if result.returncode:
        raise RuntimeError(f"{what}: {(result.stderr or result.stdout).strip()}")


def _stop(name: str) -> None:
    pidfile = RUN_DIR / f"{name}.pid"
    try:
        pid = int(pidfile.read_text().strip())
    except (OSError, ValueError):
        return
    try:
        os.kill(pid, signal.SIGTERM)
        for _ in range(30):
            time.sleep(0.1)
            os.kill(pid, 0)
    except ProcessLookupError:
        pass
    pidfile.unlink(missing_ok=True)


def _reset_interface() -> None:
    for name in ("udhcpc", "wpa_supplicant", "hostapd", "dnsmasq"):
        _stop(name)
    run("ip", "addr", "flush", "dev", IFACE)
    _require(run("ip", "link", "set", IFACE, "up"), "wlan0 up")


def _status() -> dict[str, str]:
    result = run("wpa_cli", "-p", CTRL_DIR, "-i", IFACE, "status", timeout=5)
    if result.returncode:
        return {}
    return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)


def _ipv4() -> str | None:
    result = run("ip", "-4", "-o", "addr", "show", "dev", IFACE, timeout=5)
    match = re.search(r"\binet (\d+\.\d+\.\d+\.\d+)/", result.stdout)
    return match.group(1) if match else None


def connected() -> bool:
    status = _status()
    return (status.get("wpa_state") == "COMPLETED" and status.get("mode", "station") == "station"
            and _ipv4() not in (None, AP_IP))


def saved_wifi_profile_exists() -> bool:
    try:
        return "network={" in CONF.read_text()
    except OSError:
        return False


def network_config(ssid: str, password: str) -> str:
    """A wpa_supplicant.conf for one network, without the passphrase in clear."""
    lines = [f"ctrl_interface={CTRL_DIR}", f"country={COUNTRY}", "", "network={",
             f"\tssid={ssid.encode().hex()}", "\tscan_ssid=1"]
    if not password:
        lines.append("\tkey_mgmt=NONE")
    else:
        if len(password) == 64 and re.fullmatch(r"[0-9a-fA-F]{64}", password):
            psk = password.lower()  # Already the raw 256-bit key.
        else:
            psk = hashlib.pbkdf2_hmac("sha1", password.encode(), ssid.encode(), 4096, 32).hex()
        lines += ["\tkey_mgmt=WPA-PSK", f"\tpsk={psk}"]
    return "\n".join(lines + ["}", ""])


def start_station(conf: Path = CONF) -> None:
    """Join the saved network and keep a DHCP lease on it, in the background."""
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    _reset_interface()
    _require(run("wpa_supplicant", "-B", "-D", "nl80211", "-i", IFACE, "-c", str(conf),
                 "-P", str(RUN_DIR / "wpa_supplicant.pid")), "wpa_supplicant")
    # -b: keep trying in the background until the network answers.
    _require(run("udhcpc", "-i", IFACE, "-b", "-S", "-x", "hostname:wignet-tv",
                 "-p", str(RUN_DIR / "udhcpc.pid")), "udhcpc")


def join_network(ssid: str, password: str) -> None:
    """Switch the radio from the setup AP to `ssid`; keep it only if it works."""
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    candidate = CONF.with_name(CONF.name + ".new")
    candidate.write_text(network_config(ssid, password))
    os.chmod(candidate, 0o600)
    start_station(candidate)
    deadline = time.monotonic() + JOIN_SECONDS
    while time.monotonic() < deadline:
        if connected():
            os.replace(candidate, CONF)
            return
        time.sleep(1)
    _stop("udhcpc")
    _stop("wpa_supplicant")
    candidate.unlink(missing_ok=True)
    raise RuntimeError("connect")


def parse_iw_scan(raw: str) -> str:
    """Turn `iw scan` output into nmcli's terse SSID:SECURITY:SIGNAL lines."""
    lines = []

    def flush(entry):
        if entry and entry.get("ssid"):
            ssid = entry["ssid"].replace("\\", "\\\\").replace(":", "\\:")
            lines.append(f"{ssid}:{entry.get('security', '')}:{entry.get('signal', 0)}")

    entry = None
    for line in raw.splitlines():
        if line.startswith("BSS "):
            flush(entry)
            entry = {}
            continue
        if entry is None:
            continue
        text = line.strip()
        if text.startswith("SSID:"):
            value = text[5:].strip()
            # iw escapes non-printable bytes as \xHH.
            raw_bytes = re.sub(rb"\\x([0-9a-fA-F]{2})",
                               lambda m: bytes([int(m.group(1), 16)]), value.encode())
            entry["ssid"] = raw_bytes.decode("utf-8", "replace")
        elif text.startswith("signal:"):
            dbm = float(text.split()[1])
            entry["signal"] = max(0, min(100, int(2 * (dbm + 100))))
        elif text.startswith(("RSN:", "WPA:")):
            entry["security"] = "WPA2" if text.startswith("RSN") else "WPA"
        elif text.startswith("capability:") and "Privacy" in text:
            entry.setdefault("security", "WEP")
    flush(entry)
    return "\n".join(lines)


def scan_raw() -> str:
    global _last_scan
    result = run("iw", "dev", IFACE, "scan", timeout=25)
    if result.returncode == 0:
        _last_scan = parse_iw_scan(result.stdout)
    return _last_scan


def start_hotspot(ap_name: str) -> None:
    """Open the setup network, with DHCP and every name resolving to the portal."""
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    _reset_interface()
    scan_raw()  # Before the AP takes the radio: the portal lists these.
    _require(run("ip", "addr", "add", f"{AP_IP}/24", "dev", IFACE), "AP address")
    conf = RUN_DIR / "hostapd.conf"
    conf.write_text("\n".join([
        f"interface={IFACE}", "driver=nl80211", f"ssid={ap_name}", "hw_mode=g", "channel=6",
        f"country_code={COUNTRY}", "auth_algs=1", "ignore_broadcast_ssid=0", "",
    ]))
    _require(run("hostapd", "-B", "-P", str(RUN_DIR / "hostapd.pid"), str(conf)), "hostapd")
    _require(run("dnsmasq", "--conf-file=/dev/null", f"--interface={IFACE}", "--bind-interfaces",
                 f"--listen-address={AP_IP}", "--no-resolv", "--no-hosts",
                 "--dhcp-range=192.168.4.10,192.168.4.100,255.255.255.0,1h",
                 f"--dhcp-option=option:router,{AP_IP}",
                 f"--address=/#/{AP_IP}",
                 f"--dhcp-leasefile={RUN_DIR / 'dnsmasq.leases'}",
                 f"--pid-file={RUN_DIR / 'dnsmasq.pid'}"), "dnsmasq")
