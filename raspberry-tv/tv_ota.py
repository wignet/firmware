"""Over-the-air updates for the Buildroot Wignet TV (fablamenas/wignet#322).

The contract is the ESP32's (docs/ota.md on the server): a signed `otaOffer`
on `/schedule`, bound to this poll's `on=` nonce; `ota=`/`oc=` reports; a
signed `otaConfirm` while the new image is pending.  What differs is the
Raspberry Pi's A/B mechanism:

- The SD card has two root slots, p2 and p3.  The boot partition keeps the
  shared firmware (bootcode.bin, start.elf) at its top, and each slot's
  kernel, device tree, overlays and cmdline.txt in a directory named after
  it.  `config.txt` selects the running slot with `os_prefix=pN/`.
- An offer's image is the root filesystem (ext4) of the other slot, sent
  xz-compressed; `sha256` and `size` are those of the uncompressed image, the
  bytes the slot holds.  Its `/boot` carries that slot's kernel files.
- After the download, the slot's `/boot` is copied to `pN/` on the boot
  partition and `tryboot.txt` (that image's config.txt, `os_prefix=pN/`) is
  written; the Pi reboots once with the firmware's tryboot flag.
- Confirmed, `tryboot.txt` becomes `config.txt`.  Any other reboot (the
  deadline below, S00wignet's guard, a kernel panic, a power cut) loads
  `config.txt`, so the previous slot.

Measured on the Pi Zero W, firmware of 2025-12-08 (#322): the flag is
one-shot, and ignored when `tryboot.txt` is missing.
"""
import base64
import ctypes
import ctypes.util
import fcntl
import hashlib
import json
import lzma
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

HW = "raspberry_pi_hdmi"
URL_PREFIX = "https://api.wignet.app/firmware/"
# The server's public keys, SubjectPublicKeyInfo DER as hex: the same list as
# esp32/src/ota_keys.h.  `python -m backoffice.ota` prints the key and its id.
PUBLIC_KEYS_HEX = (
    # Production signing key, id 8675e48b.
    "3059301306072a8648ce3d020106082a8648ce3d03010703420004d53bff3a738a2a87787245f1c3696a72db2834"
    "2b52bc91ba449b4b49022df9e4a80729ff833807069ab07c0c8467fa2470649e4b7972e28b5dbca278fc4c2a21",
)
KEYS = {hashlib.sha256(bytes.fromhex(key)).hexdigest()[:8]: bytes.fromhex(key)
        for key in PUBLIC_KEYS_HEX}

RESPONSE_LIMIT_SECONDS = 60
PENDING_POLL_SECONDS = 30
# Measured from the kernel's start (/proc/uptime), so a client restart does
# not extend it.  S00wignet's guard reboots a minute later in any case.
PENDING_DEADLINE_SECONDS = 600
REPORT_POLLS = 20
MAX_COUNTER = 2 ** 53 - 1
# No wall clock before this one can judge `expires` (2026-01-01).
PLAUSIBLE_TIME = 1767225600
CONFIRMED_MARK = "/run/wignet-ota-confirmed"

REASONS = ("download", "hash", "size", "write", "power", "chip")
HEX64 = re.compile(r"[0-9a-f]{64}")
NONCE = re.compile(r"[0-9a-f]{32}")
SLOTS = ("p2", "p3")
CHUNK = 64 * 1024


def log(message):
    print(f"[OTA] {message}", file=sys.stderr)


class LibCrypto:
    """ECDSA P-256 / SHA-256 verification with the system's libcrypto.

    Python's own `ssl` already loads it, so verifying needs no package the
    image does not have (no `cryptography`, no `openssl` binary)."""

    def __init__(self):
        names = ("libcrypto.so.3", "libcrypto.so.1.1", ctypes.util.find_library("crypto"))
        for name in filter(None, names):
            try:
                self.lib = ctypes.CDLL(name)
                break
            except OSError:
                continue
        else:
            raise OSError("libcrypto not found")
        lib, ptr = self.lib, ctypes.c_void_p
        lib.d2i_PUBKEY.argtypes = [ptr, ctypes.POINTER(ptr), ctypes.c_long]
        lib.d2i_PUBKEY.restype = ptr
        lib.EVP_PKEY_free.argtypes = [ptr]
        lib.EVP_MD_CTX_new.restype = ptr
        lib.EVP_MD_CTX_free.argtypes = [ptr]
        lib.EVP_sha256.restype = ptr
        lib.EVP_DigestVerifyInit.argtypes = [ptr, ptr, ptr, ptr, ptr]
        lib.EVP_DigestVerify.argtypes = [ptr, ctypes.c_char_p, ctypes.c_size_t,
                                         ctypes.c_char_p, ctypes.c_size_t]

    def verify(self, der, message, signature):
        lib = self.lib
        buffer = ctypes.create_string_buffer(der, len(der))
        cursor = ctypes.c_void_p(ctypes.addressof(buffer))
        key = lib.d2i_PUBKEY(None, ctypes.byref(cursor), len(der))
        if not key:
            return False
        context = lib.EVP_MD_CTX_new()
        try:
            if lib.EVP_DigestVerifyInit(context, None, lib.EVP_sha256(), None, key) != 1:
                return False
            return lib.EVP_DigestVerify(context, signature, len(signature),
                                        message, len(message)) == 1
        finally:
            lib.EVP_MD_CTX_free(context)
            lib.EVP_PKEY_free(key)


def verify_signature(key_id, message, signature_b64, keys=KEYS, crypto=None):
    """True when `signature_b64` is a known key's signature of `message`."""
    der = keys.get(key_id)
    if der is None or not isinstance(signature_b64, str) or len(signature_b64) > 160:
        return False
    try:
        signature = base64.b64decode(signature_b64, validate=True)
    except ValueError:
        return False
    try:
        crypto = crypto or LibCrypto()
    except OSError as exc:
        log(f"cannot verify signatures ({exc})")
        return False
    return crypto.verify(der, message.encode(), signature)


def offer_canonical(mac, nonce, offer):
    """The lines an offer is signed over (docs/ota.md, "The offer").  MAC
    and nonce are this board's and this poll's, never the offer's fields."""
    return "\n".join(("wignet-ota1", mac, offer["hw"], offer["sha256"], str(offer["size"]),
                      str(offer["counter"]), str(offer["expires"]), nonce,
                      offer["powerSource"], "true" if offer["powerOverride"] else "false",
                      offer["url"]))


def confirm_canonical(mac, sha256, counter, nonce):
    return "\n".join(("wignet-ota-confirm1", mac, HW, sha256, str(counter), nonce))


def _is_int(value):
    return type(value) is int


def read_offer(raw):
    """The offer's fields when each has the contract's type, else None."""
    if not isinstance(raw, dict):
        return None
    text = ("id", "url", "sha256", "hw", "nonce", "powerSource", "key", "sig")
    if not all(isinstance(raw.get(name), str) and len(raw[name]) <= 320 for name in text):
        return None
    if not all(_is_int(raw.get(name)) for name in ("size", "counter", "expires")):
        return None
    if type(raw.get("powerOverride")) is not bool:
        return None
    if any("\n" in raw[name] or "\r" in raw[name] for name in text):
        return None
    return raw


def url_allowed(url):
    """HTTPS on api.wignet.app, port 443, under /firmware/, nothing else."""
    if not url.startswith(URL_PREFIX):
        return False
    parts = urlsplit(url)
    return (parts.scheme == "https" and parts.hostname == "api.wignet.app"
            and parts.port in (None, 443) and parts.username is None
            and parts.path.startswith("/firmware/"))


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def download(url):
    """The firmware response, TLS verified by default, redirects refused."""
    return build_opener(NoRedirect).open(Request(url, headers={"Accept": "application/x-xz"}),
                                         timeout=30)


class Failure(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def write_image(response, out, size, sha256):
    """Decompress `response` into `out`, hashing what lands in the slot."""
    digest = hashlib.sha256()
    decompressor = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=64 * 1024 * 1024)
    written = 0
    received = 0
    try:
        while not decompressor.eof:
            if decompressor.needs_input:
                chunk = response.read(CHUNK)
                if not chunk:
                    raise Failure("download")
                received += len(chunk)
                if received > size:  # xz never makes a filesystem image larger
                    raise Failure("size")
            else:
                chunk = b""
            data = decompressor.decompress(chunk, max_length=CHUNK)
            if written + len(data) > size:
                raise Failure("size")
            digest.update(data)
            try:
                out.write(data)
            except OSError as exc:
                raise Failure("write") from exc
            written += len(data)
    except (lzma.LZMAError, EOFError, OSError) as exc:
        raise Failure("download") from exc
    if written != size:
        raise Failure("size")
    if digest.hexdigest() != sha256:
        raise Failure("hash")


def set_os_prefix(config, slot):
    """config.txt for `slot`: its `os_prefix=` line pointing at `pN/`."""
    lines = config.splitlines()
    found = [i for i, line in enumerate(lines) if line.strip().startswith("os_prefix=")]
    if len(found) != 1:
        raise Failure("write")
    lines[found[0]] = f"os_prefix={slot}/"
    return "\n".join(lines) + "\n"


class Platform:
    """The SD card, the boot partition and the firmware's tryboot flag."""

    disk = "/dev/mmcblk0"
    boot = "/dev/mmcblk0p1"
    work = Path("/tmp/wignet-ota")

    def device(self, slot):
        return f"{self.disk}{slot}"

    def slot_bytes(self, slot):
        return int(Path(f"/sys/class/block/mmcblk0{slot}/size").read_text()) * 512

    def booted_slot(self):
        match = re.search(r"(?:^|\s)root=/dev/mmcblk0(p[23])(?:\s|$)",
                          Path("/proc/cmdline").read_text())
        return match.group(1) if match else None

    def tryboot(self):
        try:
            flag = Path("/proc/device-tree/chosen/bootloader/tryboot").read_bytes()
        except OSError:
            return False
        return len(flag) == 4 and struct.unpack(">I", flag)[0] != 0

    def uptime(self):
        return float(Path("/proc/uptime").read_text().split()[0])

    def open_slot(self, slot):
        return open(self.device(slot), "r+b", buffering=0)

    def image_hash(self, slot, size):
        digest = hashlib.sha256()
        with open(self.device(slot), "rb") as image:
            left = size
            while left:
                chunk = image.read(min(1024 * 1024, left))
                if not chunk:
                    break
                digest.update(chunk)
                left -= len(chunk)
        return digest.hexdigest()

    @contextmanager
    def mounted(self, device, name, fstype, options):
        point = self.work / name
        point.mkdir(parents=True, exist_ok=True)
        subprocess.run(["mount", "-t", fstype, "-o", options, device, str(point)], check=True)
        try:
            yield point
        finally:
            os.sync()
            subprocess.run(["umount", str(point)], check=False)

    def install_boot(self, slot):
        """Copy the written slot's /boot to `slot/` on the boot partition,
        then write tryboot.txt for it."""
        # noload: a read-only mount that never replays the journal, so the
        # slot keeps exactly the bytes its hash was checked over.
        with self.mounted(self.device(slot), "slot", "ext4", "ro,noload") as root, \
                self.mounted(self.boot, "boot", "vfat", "rw") as boot:
            source = root / "boot"
            names = {path.name for path in source.iterdir()} if source.is_dir() else set()
            if not {"zImage", "cmdline.txt", "config.txt"} <= names \
                    or not any(name.endswith(".dtb") for name in names):
                log(f"slot {slot} has no complete /boot ({sorted(names)})")
                raise Failure("write")
            staging = boot / f"{slot}.new"
            shutil.rmtree(staging, ignore_errors=True)
            copy_tree(source, staging, skip={"config.txt"})
            cmdline = (source / "cmdline.txt").read_text()
            cmdline, count = re.subn(r"root=/dev/mmcblk0p[23]", f"root={self.device(slot)}", cmdline)
            if count != 1:
                raise Failure("write")
            (staging / "cmdline.txt").write_text(cmdline)
            shutil.rmtree(boot / slot, ignore_errors=True)
            os.rename(staging, boot / slot)
            (boot / "tryboot.txt").write_text(set_os_prefix((source / "config.txt").read_text(), slot))

    def confirm(self, slot):
        """tryboot.txt becomes config.txt: the next boots stay on `slot`."""
        with self.mounted(self.boot, "boot", "vfat", "rw") as boot:
            config = (boot / "tryboot.txt").read_text()
            if f"\nos_prefix={slot}/\n" not in "\n" + config:
                raise Failure("write")
            temporary = boot / "config.new"
            temporary.write_text(config)
            os.replace(temporary, boot / "config.txt")
            (boot / "tryboot.txt").unlink()

    def reboot_tryboot(self):
        """Reboot once with the tryboot flag, through init's clean shutdown.

        The flag is set over the firmware mailbox first: the kernel only
        overwrites it when the reboot itself names flags, which BusyBox's
        `reboot` cannot."""
        os.sync()
        try:
            set_tryboot_flag()
        except OSError as exc:
            log(f"mailbox refused the tryboot flag ({exc}); rebooting through the kernel")
            restart2(b"0 tryboot")
            return
        subprocess.run(["reboot"], check=False)

    def reboot(self):
        os.sync()
        subprocess.run(["reboot"], check=False)


def copy_tree(source, target, skip=()):
    """Plain copies: vfat keeps neither owners nor modes."""
    target.mkdir()
    for path in source.iterdir():
        if path.name in skip:
            continue
        if path.is_dir():
            copy_tree(path, target / path.name)
        else:
            shutil.copyfile(path, target / path.name)


# The VideoCore mailbox (/dev/vcio), as vcmailbox uses it.
IOCTL_MBOX_PROPERTY = 0xC0046400
TAG_SET_REBOOT_FLAGS = 0x00038064


def set_tryboot_flag():
    import array
    buffer = array.array("I", [7 * 4, 0, TAG_SET_REBOOT_FLAGS, 4, 4, 1, 0])
    with open("/dev/vcio", "rb", buffering=0) as mailbox:
        fcntl.ioctl(mailbox.fileno(), IOCTL_MBOX_PROPERTY, buffer, True)
    if buffer[1] != 0x80000000:
        raise OSError(f"mailbox answered {buffer[1]:#x}")


def restart2(argument):
    """reboot(2) with an argument: no clean shutdown, only a sync before."""
    libc = ctypes.CDLL(None, use_errno=True)
    os.sync()
    sys_reboot = 88  # ARM EABI
    libc.syscall(sys_reboot, 0xfee1dead, 672274793, 0xA1B2C3D4, argument)
    raise OSError(ctypes.get_errno(), "reboot(RESTART2) failed")


class TvOta:
    """The attempt record on /data, the pending state and the poll fields."""

    def __init__(self, mac, state_dir, platform=None, keys=KEYS, crypto=None,
                 clock=time.monotonic, wall=time.time, opener=download):
        self.mac = mac
        self.path = Path(state_dir) / "ota.json"
        self.platform = platform or Platform()
        self.keys = keys
        self.crypto = crypto
        self.clock = clock
        self.wall = wall
        self.opener = opener
        self.record = self._load()
        self.pending = False
        self.running_hash = None
        self.nonce = None
        self.nonce_at = 0.0

    # The record: counter, sha256, size, target, previous, phase, reason,
    # reports.  Phases: installing, selected, confirmed, failed, rolledback.
    def _load(self):
        try:
            record = json.loads(self.path.read_text())
        except (OSError, ValueError):
            return {"counter": 0}
        return record if isinstance(record, dict) and _is_int(record.get("counter")) else {"counter": 0}

    def _save(self):
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.path.with_name(self.path.name + ".tmp")
            with open(temporary, "w") as out:
                json.dump(self.record, out)
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, self.path)
            return True
        except OSError as exc:
            log(f"cannot save the attempt record ({exc})")
            return False

    def _finish(self, phase, reason=None):
        self.record.update(phase=phase, reason=reason, reports=REPORT_POLLS)
        self._save()

    @property
    def short_id(self):
        return self.record.get("sha256", "")[:16]

    def boot(self):
        """Reconcile the record with the slot this boot came from."""
        record, platform = self.record, self.platform
        phase = record.get("phase")
        booted = platform.booted_slot()
        if phase == "installing":
            log(f"installation of {self.short_id} was interrupted")
            self._finish("failed", "write")
        elif phase == "selected":
            if booted == record.get("target") and platform.tryboot():
                self.pending = True
                self.running_hash = platform.image_hash(booted, record["size"])
                log(f"running {self.short_id} on {booted}, pending confirmation")
            elif booted == record.get("target"):
                # Confirmed before a reboot that came ahead of the record.
                self._finish("confirmed")
            else:
                log(f"{self.short_id} did not stay: back on {booted}")
                self._finish("rolledback")
        if not self.pending:
            mark_confirmed()

    def query(self):
        """This poll's OTA fields: a fresh nonce, then what there is to say."""
        self.nonce = os.urandom(16).hex()
        self.nonce_at = self.clock()
        fields = {"on": self.nonce}
        record = self.record
        if self.pending:
            fields["oh"] = self.running_hash
            fields["ota"] = f"pending:{self.short_id}"
        elif record.get("phase") in ("confirmed", "failed", "rolledback") \
                and record.get("reports", 0) > 0:
            state = record["phase"]
            fields["ota"] = f"{state}:{self.short_id}" + (
                f":{record['reason']}" if state == "failed" else "")
            record["reports"] -= 1
            self._save()
        if "ota" in fields:
            fields["oc"] = record["counter"]
        return fields

    def answer(self, payload, seconds):
        """Act on a /schedule answer; the delay until the next poll."""
        nonce, self.nonce = self.nonce, None  # one answer per poll
        if nonce is not None and self.clock() - self.nonce_at <= RESPONSE_LIMIT_SECONDS \
                and isinstance(payload, dict):
            if self.pending and "otaConfirm" in payload:
                self._confirm(payload["otaConfirm"], nonce)
            elif not self.pending and "otaOffer" in payload:
                self._offer(payload["otaOffer"], nonce)
        if self.pending:
            if self.platform.uptime() >= PENDING_DEADLINE_SECONDS:
                log(f"{self.short_id} not confirmed in time; rebooting into the previous slot")
                self.platform.reboot()
            return min(seconds, PENDING_POLL_SECONDS)
        return seconds

    def _verify(self, key_id, message, signature):
        return verify_signature(key_id, message, signature, keys=self.keys, crypto=self.crypto)

    def _offer(self, raw, nonce):
        offer = read_offer(raw)
        if offer is None:
            return log("offer refused: unreadable fields")
        log(f"offer {offer['id']} seen: counter {offer['counter']}, last {self.record['counter']}")
        refusal = self._refusal(offer, nonce)
        if refusal:
            return log(f"offer refused: {refusal}")
        log(f"offer {offer['id']} accepted (counter {offer['counter']})")
        self._install(offer)

    def _refusal(self, offer, nonce):
        if offer["key"] not in self.keys:
            return "unknown key"
        if offer["hw"] != HW:
            return "hw is not this board"
        if not url_allowed(offer["url"]):
            return "url outside /firmware/"
        if not self._verify(offer["key"], offer_canonical(self.mac, nonce, offer), offer["sig"]):
            return "signature"
        if not 0 < offer["counter"] <= MAX_COUNTER:
            return "counter out of range"
        if offer["counter"] <= self.record["counter"]:
            return "counter not above the last attempt"
        if not HEX64.fullmatch(offer["sha256"]) or offer["id"] != offer["sha256"][:16]:
            return "id is not the hash"
        now = self.wall()
        if now > PLAUSIBLE_TIME and now >= offer["expires"]:
            return "expired"
        booted = self.platform.booted_slot()
        if booted not in SLOTS:
            return "not running from an A/B slot"
        if not 0 < offer["size"] <= self.platform.slot_bytes(other(booted)):
            return "size exceeds the slot"
        return None

    def _install(self, offer):
        platform = self.platform
        previous = platform.booted_slot()
        target = other(previous)
        # A TV is mains-powered (pwr=ext): no battery to check.
        self.record = {"counter": offer["counter"], "sha256": offer["sha256"],
                       "size": offer["size"], "target": target, "previous": previous,
                       "phase": "installing"}
        if not self._save():
            return log("could not persist the attempt; not installing")
        try:
            try:
                response = self.opener(offer["url"])
            except OSError as exc:
                raise Failure("download") from exc
            with response, platform.open_slot(target) as out:
                write_image(response, out, offer["size"], offer["sha256"])
                os.fsync(out.fileno())
            platform.install_boot(target)
        except Failure as failure:
            log(f"installation failed: {failure.reason}")
            return self._finish("failed", failure.reason)
        except (OSError, subprocess.CalledProcessError) as exc:
            log(f"installation failed: write ({exc})")
            return self._finish("failed", "write")
        self.record["phase"] = "selected"
        if not self._save():
            log("could not persist the selected slot")
        log(f"rebooting into {target} with tryboot")
        platform.reboot_tryboot()

    def _confirm(self, raw, nonce):
        if not isinstance(raw, dict):
            return
        sha256, counter = raw.get("sha256"), raw.get("counter")
        if not (_is_int(counter) and counter == self.record["counter"]
                and raw.get("nonce") == nonce and sha256 == self.running_hash
                and isinstance(raw.get("key"), str) and isinstance(raw.get("sig"), str)):
            return log("confirmation ignored: not for this attempt or poll")
        if not self._verify(raw["key"], confirm_canonical(self.mac, sha256, counter, nonce), raw["sig"]):
            return log("confirmation ignored: signature")
        try:
            self.platform.confirm(self.record["target"])
        except (Failure, OSError, subprocess.CalledProcessError) as exc:
            return log(f"could not switch config.txt ({exc}); still pending")
        self.pending = False
        self._finish("confirmed")
        mark_confirmed()
        log(f"{self.short_id} confirmed on {self.record['target']}")


def other(slot):
    return "p3" if slot == "p2" else "p2"


def mark_confirmed(path=CONFIRMED_MARK):
    """Tell S00wignet's guard not to reboot this boot."""
    try:
        Path(path).touch()
    except OSError:
        pass


def from_environment(mac, state_dir):
    """A/B updates on the Buildroot image only (WIGNET_TV_OTA=ab)."""
    if os.environ.get("WIGNET_TV_OTA", "").strip().lower() != "ab":
        return None
    ota = TvOta(mac, state_dir)
    try:
        ota.boot()
    except OSError as exc:
        log(f"disabled ({exc})")
        return None
    return ota
