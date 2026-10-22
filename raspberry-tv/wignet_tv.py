#!/usr/bin/env python3
"""A deliberately small HDMI client for an RGB Wignet.

The server selects the active event/slide and renders its PNG.  This client
only declares its hardware, fetches that already-prepared frame, displays it
when it changed, and follows the cadence returned by ``/schedule``.  It has
no local event, slide, or content-rendering logic.  The sounds ``/schedule``
lists are rung through the TV's speakers by ``sound_player`` (on by default,
``WIGNET_SOUND=off`` for a monitor without speakers).
"""
import argparse
import array
import io
import json
import logging
import os
import re
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from cec import CEC
from tv_power import TVPower

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # Unit tests exercise the poll contract without a display.
    Image = None

try:
    import numpy as np
except ImportError:  # Keep the polling contract importable without display packages.
    np = None


DEFAULT_HOST = "https://api.wignet.app"
# rgb: shows full sRGB PNGs; screen_size1: w/h below are the real buffer size;
# power_report1: sends pwr= (always ext: a TV is plugged in).
CAPS = "v1:rgb,screen_size1,power_report1"
# The TV's speakers ring the Wignet's sounds: alarms, one-off events and
# pushed chimes (sound_list1: the whole upcoming list; tune_mml1: MML tunes).
SOUND_CAPS = CAPS + ",wake_alarm,audio_out,sound_list1,tune_mml1"
FALLBACK_SECONDS = 60
MIN_SECONDS = 1
MAX_SECONDS = 86400
MAC = re.compile(r"^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$", re.I)
EVENT_OCCURRENCE = re.compile(r"^[0-9a-f]{24}$")
SUPPORT_LOG = "support.log"
SUPPORT_LOG_BYTES = 64 * 1024


class SupportLog:
    """Copy what the client writes to stderr into a small file that survives a reboot.

    On the Buildroot image syslog lives in RAM, so a power cut erased the very
    lines support needs ("did it ring, did the TV wake?").  The client writes
    to stderr only what matters (sounds, CEC, errors, a crash's traceback), a
    few lines a day, so the SD card sees almost no writes.  Bounded at
    `max_bytes`, plus one previous file `support.log.1`.  A file that cannot be
    written never stops the client.
    """

    def __init__(self, stream, path, max_bytes=SUPPORT_LOG_BYTES, clock=time.time):
        self.stream = stream
        self.path = Path(path)
        self.max_bytes = max_bytes
        self.clock = clock
        self.pending = ""
        self.lock = threading.Lock()

    def write(self, text):
        self.stream.write(text)
        with self.lock:
            self.pending += text
            *lines, self.pending = self.pending.split("\n")
            if lines:
                self._append(lines)
        return len(text)

    def flush(self):
        self.stream.flush()

    def __getattr__(self, name):
        return getattr(self.stream, name)

    def _append(self, lines):
        stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(self.clock()))
        try:
            if self.path.stat().st_size >= self.max_bytes:
                os.replace(self.path, self.path.with_name(self.path.name + ".1"))
        except OSError:
            pass
        try:
            with open(self.path, "a", encoding="utf-8") as log:
                log.write("".join(f"{stamp} {line}\n" for line in lines))
        except OSError:
            pass


# Match the Wignet site and the ESP setup portal tokens.  The two mark
# polygons below are the same 32x16 paths as website-wignet's brand SVG, so
# the TV can show the brand before networking and without loading any asset.
PAPER = (247, 242, 232)       # #f7f2e8 / --app-background
INK = (51, 32, 33)            # #332021 / --foreground
MUTED = (107, 89, 84)         # #6b5954 / --muted-foreground
BORDER = (235, 229, 219)      # #ebe5db / --border
CORAL = (186, 74, 79)         # #ba4a4f / --highlight


def _rgb565(color):
    red, green, blue = color
    pixel = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
    return bytes((pixel & 0xff, pixel >> 8))


def progress_row(width, segment_width, offset, track_color=BORDER, segment_color=CORAL):
    """Build one RGB565 row for the small, indeterminate startup bar."""
    segment_width = max(0, min(width, segment_width))
    offset = max(0, min(width - segment_width, offset))
    return (_rgb565(track_color) * offset
            + _rgb565(segment_color) * segment_width
            + _rgb565(track_color) * (width - offset - segment_width))


@dataclass(frozen=True)
class ProgressRect:
    x: int
    y: int
    width: int
    height: int


@dataclass(frozen=True)
class Settings:
    host: str
    mac: str
    width: int
    height: int
    caps: str = CAPS
    fw: str = ""

    def image_query(self):
        return urlencode({"mac": self.mac, "w": self.width, "h": self.height})

    def schedule_query(self):
        query = {"mac": self.mac, "w": self.width, "h": self.height,
                 "caps": self.caps, "pwr": "ext", "hw": "raspberry_pi_hdmi"}
        if self.fw:
            query["fw"] = self.fw
        return urlencode(query)

    @property
    def image_url(self):
        return f"{self.host}/pic/4displayB5.png?{self.image_query()}"

    @property
    def schedule_url(self):
        return f"{self.host}/schedule?{self.schedule_query()}"


FIRMWARE_VERSION = re.compile(r"[0-9a-f]{7,40}(-dirty\.[0-9a-f]{8})?|unknown")


def installed_version():
    """The version tv_version.py stamped at install time; empty when absent or malformed."""
    try:
        raw = Path(__file__).with_name("VERSION").read_text().strip()
    except OSError:
        return ""
    return raw if FIRMWARE_VERSION.fullmatch(raw) else ""


def interface_mac():
    """Use a physical interface's stable address, never a generated id."""
    names = ("wlan0", "eth0")
    paths = [Path("/sys/class/net") / name / "address" for name in names]
    paths += sorted(Path("/sys/class/net").glob("*/address"))
    for path in paths:
        try:
            value = path.read_text().strip().lower()
        except OSError:
            continue
        if MAC.fullmatch(value) and value != "00:00:00:00:00:00":
            return value
    raise ValueError("could not find a physical network MAC")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("WIGNET_HOST", DEFAULT_HOST).rstrip("/"))
    parser.add_argument("--mac", default=os.environ.get("WIGNET_MAC"))
    # Unset: the HDMI mode's own size, which the server then renders at.
    parser.add_argument("--width", type=int, default=int(os.environ.get("WIGNET_WIDTH") or 0) or None)
    parser.add_argument("--height", type=int, default=int(os.environ.get("WIGNET_HEIGHT") or 0) or None)
    parser.add_argument("--state-dir", default=os.environ.get("WIGNET_STATE_DIR", "/var/lib/wignet-tv"),
                        help="directory holding the last displayed image ETag")
    parser.add_argument("--once", action="store_true", help="fetch once and exit (for setup diagnostics)")
    args = parser.parse_args(argv)
    mac = (args.mac or interface_mac()).lower()
    if not MAC.fullmatch(mac):
        parser.error("--mac must be a colon-separated MAC address")
    if not args.host.startswith("https://"):
        parser.error("--host must use HTTPS")
    if (args.width is None) != (args.height is None):
        parser.error("give both --width and --height, or neither")
    return args.host, mac, args.width, args.height, args.state_dir, args.once


def checked_size(width, height):
    if not (1 <= width <= 4096 and 1 <= height <= 4096 and width * height <= 4_000_000):
        raise SystemExit("display size must be positive and at most 4,000,000 pixels")
    return width, height


def load_etag(state_dir):
    """Return the last successfully displayed ETag, if this is not a cold boot."""
    try:
        value = (Path(state_dir) / "image.etag").read_text(encoding="ascii").strip()
    except OSError:
        return None
    return value or None


def save_etag(state_dir, etag):
    """Atomically retain an ETag only after its frame reached the display."""
    if not etag:
        return
    directory = Path(state_dir)
    try:
        directory.mkdir(parents=True, exist_ok=True)
        temporary = directory / "image.etag.tmp"
        temporary.write_text(etag + "\n", encoding="ascii")
        temporary.replace(directory / "image.etag")
    except OSError as exc:
        print(f"cannot persist image ETag ({exc}); continuing without cache", file=sys.stderr)


def splash_cache_name(width, height, stride):
    """The raw RGB565 splash that boot-splash.sh shows at the next boot."""
    return f"splash-{width}x{height}-{stride}.rgb565"


def save_splash(state_dir, display, frame):
    """Keep the exact splash frame so the next boot can show it before Python
    and Pillow are loaded.  Rewritten only when the design or screen changed."""
    path = Path(state_dir) / splash_cache_name(display.width, display.height, display.stride)
    try:
        if path.is_file() and path.read_bytes() == frame:
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        for stale in path.parent.glob("splash-*.rgb565"):
            stale.unlink()
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_bytes(frame)
        temporary.replace(path)
    except OSError as exc:
        print(f"cannot cache the boot splash ({exc})", file=sys.stderr)


def get_image(settings, etag=None):
    headers = {"Accept": "image/png"}
    if etag:
        headers["If-None-Match"] = etag
    request = Request(settings.image_url, headers=headers)
    try:
        with urlopen(request, timeout=20) as response:
            if response.headers.get_content_type() != "image/png":
                raise ValueError("server did not return a PNG")
            return (response.read(), response.headers.get("ETag"), False,
                    event_wake(response.headers))
    except HTTPError as exc:
        if exc.code == 304:
            return None, etag, True, event_wake(exc.headers)
        raise


def event_wake(headers):
    """Read the optional active-event CEC hint; ordinary slides have none."""
    if headers is None:
        return None
    occurrence = headers.get("X-Wignet-Event-Occurrence")
    delay = headers.get("X-Wignet-Event-Wake-After")
    window_remaining = headers.get("X-Wignet-Event-Window-Remaining")
    if not occurrence or not EVENT_OCCURRENCE.fullmatch(occurrence):
        return None
    try:
        delay = int(delay)
    except (TypeError, ValueError):
        return None
    if not 0 <= delay <= 3600:
        return None
    event = {"occurrence": occurrence, "delay_seconds": delay}
    if window_remaining is not None:
        try:
            window_remaining = int(window_remaining)
        except (TypeError, ValueError):
            return None
        if not delay <= window_remaining <= 3600 + 24 * 3600:
            return None
        event["window_remaining_seconds"] = window_remaining
    return event


class EventWakeScheduler:
    """Wake the TV once at the scheduled time of an active display event."""

    def __init__(self, power, timer_factory=threading.Timer):
        self.power = power
        self.timer_factory = timer_factory
        self.lock = threading.Lock()
        self.pending = None
        self.timer = None
        self.last_woken = None

    def update(self, event):
        with self.lock:
            if event is None:
                self._cancel_locked()
                return
            occurrence = event["occurrence"]
            if occurrence == self.last_woken or occurrence == self.pending:
                return
            self._cancel_locked()
            timer = self.timer_factory(event["delay_seconds"], self._fire,
                                       args=(occurrence, event.get("window_remaining_seconds"),
                                             event["delay_seconds"]))
            timer.daemon = True
            self.pending = occurrence
            self.timer = timer
            timer.start()

    def _cancel_locked(self):
        if self.timer is not None:
            self.timer.cancel()
        self.timer = None
        self.pending = None

    def _fire(self, occurrence, window_remaining=None, delay_seconds=0):
        with self.lock:
            if self.pending != occurrence:
                return
            self.pending = None
            self.timer = None
            self.last_woken = occurrence
        if window_remaining is None:
            self.power.wake_tv()
        else:
            self.power.wake_tv(hold_seconds=max(0, window_remaining - delay_seconds))


def next_seconds(settings, player=None):
    """Ask when to poll again; hand the answer's sounds to `player`."""
    try:
        with urlopen(Request(settings.schedule_url, headers={"Accept": "application/json"}), timeout=15) as response:
            payload = json.loads(response.read())
        value = payload["nextRefreshSec"]
        if type(value) is not int or not MIN_SECONDS <= value <= MAX_SECONDS:
            raise ValueError("invalid nextRefreshSec")
    except (HTTPError, URLError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f"schedule unavailable ({exc}); retrying in {FALLBACK_SECONDS}s", file=sys.stderr)
        return FALLBACK_SECONDS
    if player is not None:
        player.update(payload)
    return value


def sound_enabled():
    value = os.environ.get("WIGNET_SOUND", "on").strip().lower()
    if value in ("1", "on", "true", "yes"):
        return True
    if value in ("0", "off", "false", "no"):
        return False
    raise SystemExit("WIGNET_SOUND must be on or off")


def start_player(state_dir, power):
    """The sound thread, or None when sound is off or its packages are missing."""
    if not sound_enabled():
        return None
    try:
        import sound_player
    except ImportError as exc:  # numpy, or mml.py missing from the install
        print(f"sound disabled ({exc})", file=sys.stderr)
        return None
    player = sound_player.Player(state_dir, power,
                                 device=os.environ.get("WIGNET_AUDIO_DEVICE") or "default")
    player.start()
    return player


# Pi OS Lite ships FreeSans/FreeMono; the Buildroot image ships Liberation.
# Load the files directly: neither system is guaranteed to have Fontconfig.
SANS_FONTS = ("/usr/share/fonts/truetype/freefont/FreeSans.ttf",
              "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf")
MONO_FONTS = ("/usr/share/fonts/truetype/freefont/FreeMono.ttf",
              "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
              "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf")


def load_font(paths, size):
    for path in paths:
        if Path(path).is_file():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size)


def pack_rgb565(rgb, width, height, stride):
    """Pack 8-bit RGB rows into the framebuffer's little-endian RGB565 rows."""
    if sys.byteorder != "little":
        raise ValueError("RGB565 framebuffer output currently requires a little-endian CPU")
    if np is not None:
        channels = np.frombuffer(rgb, dtype=np.uint8).reshape(height, width, 3)
        red = channels[:, :, 0].astype(np.uint16)
        green = channels[:, :, 1].astype(np.uint16)
        blue = channels[:, :, 2].astype(np.uint16)
        pixels = (((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)) \
            .astype("<u2", copy=False).tobytes()
    else:
        pixels = array.array(
            "H",
            (((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
             for red, green, blue in zip(rgb[0::3], rgb[1::3], rgb[2::3]))
        ).tobytes()
    row_bytes = width * 2
    if stride == row_bytes:
        return pixels
    padding = b"\0" * (stride - row_bytes)
    return b"".join(pixels[y * row_bytes:(y + 1) * row_bytes] + padding
                    for y in range(height))


class Display:
    def __init__(self):
        if Image is None:
            raise SystemExit("Pillow is required; install python3-pil")
        # Decode and draw with Pillow, then write RGB565 directly to fb0: no
        # SDL, no KMS client, the same path on Pi OS Lite and the Wignet image.
        fb = Path("/sys/class/graphics/fb0")
        try:
            self.width, self.height = map(int, (fb / "virtual_size").read_text().strip().split(","))
            self.bits_per_pixel = int((fb / "bits_per_pixel").read_text().strip())
            self.stride = int((fb / "stride").read_text().strip())
            self.fd = os.open("/dev/fb0", os.O_WRONLY)
        except (OSError, ValueError) as exc:
            raise SystemExit(f"cannot open HDMI framebuffer: {exc}") from exc
        if self.bits_per_pixel != 16 or self.stride < self.width * 2:
            os.close(self.fd)
            raise SystemExit(
                f"unsupported framebuffer format: {self.width}x{self.height}, "
                f"{self.bits_per_pixel} bpp, stride {self.stride}"
            )
        self.size = (self.width, self.height)
        self.progress_rect = None
        self._progress_stop = None
        self._progress_thread = None
        self.has_page = False

    def _write_image(self, image):
        if image.size != self.size:
            raise ValueError(f"frame is {image.size}, expected {self.size}")
        frame = pack_rgb565(image.convert("RGB").tobytes(), self.width, self.height, self.stride)
        written = os.pwrite(self.fd, frame, 0)
        if written != len(frame):
            raise OSError(f"short framebuffer write: {written} of {len(frame)} bytes")
        return frame

    def splash(self):
        """Show the Wignet site-inspired startup screen before the first frame."""
        width, height = self.size
        margin_x = max(28, width // 12)
        top = max(24, height // 18)
        mark_w = max(48, height // 18)
        mark_h = mark_w // 2
        mark_x, mark_y = margin_x, top + 3
        half = mark_w // 2

        image = Image.new("RGB", self.size, PAPER)
        draw = ImageDraw.Draw(image)
        # Exact silhouette of wignet-mark-ink.svg, scaled from its 32x16 viewBox.
        draw.polygon(((mark_x + half, mark_y), (mark_x + mark_w, mark_y + mark_h),
                      (mark_x + half, mark_y + mark_h)), fill=INK)
        draw.polygon(((mark_x + half, mark_y + mark_h), (mark_x, mark_y + mark_h),
                      (mark_x + mark_w // 4, mark_y + mark_h // 2)), fill=INK)

        def line_height(font):
            ascent, descent = font.getmetrics()
            return ascent + descent

        brand_font = load_font(SANS_FONTS, max(28, height // 30))
        draw.text((mark_x + mark_w + max(12, height // 90),
                   mark_y + (mark_h - line_height(brand_font)) // 2),
                  "Wignet", font=brand_font, fill=INK)

        rule_y = top + mark_h + max(22, height // 48)
        draw.line(((margin_x, rule_y), (width - margin_x, rule_y)),
                  fill=BORDER, width=max(1, height // 900))

        kicker_font = load_font(MONO_FONTS, max(18, height // 48))
        detail_font = load_font(SANS_FONTS, max(24, height // 36))
        title_text = "Donnez une fenêtre à vos agents."
        title_size = max(30, min(height // 12, int((width - 2 * margin_x) / (len(title_text) * 0.56))))
        title_font = load_font(SANS_FONTS, title_size)
        center_y = height // 2
        kicker_y = (center_y - line_height(title_font) - line_height(detail_font)
                    - max(40, height // 16))
        title_y = kicker_y + line_height(kicker_font) + max(14, height // 48)
        detail_y = title_y + line_height(title_font) + max(18, height // 40)
        draw.text((margin_x, kicker_y), "ÉCRAN WIGNET  /  HDMI", font=kicker_font, fill=CORAL)
        draw.text((margin_x, title_y), title_text, font=title_font, fill=INK)
        draw.text((margin_x, detail_y), "Connexion à votre Wignet en cours.",
                  font=detail_font, fill=MUTED)

        bar_w = min(width - 2 * margin_x, max(180, width // 3))
        bar_h = max(8, height // 90)
        bar_y = detail_y + line_height(detail_font) + max(28, height // 24)
        self.progress_rect = ProgressRect(margin_x, bar_y, bar_w, bar_h)
        draw.rectangle((margin_x, bar_y, margin_x + bar_w - 1, bar_y + bar_h - 1), fill=BORDER)

        # The site uses short mono labels and a quiet coral accent, with no
        # extra chrome competing with the screen's eventual content.
        status_font = load_font(MONO_FONTS, max(16, height // 60))
        status_h = line_height(status_font)
        status_y = height - top - status_h
        dot_size = max(6, height // 120)
        dot_y = status_y + (status_h - dot_size) // 2
        draw.rectangle((margin_x, dot_y, margin_x + dot_size - 1, dot_y + dot_size - 1), fill=CORAL)
        draw.text((margin_x + dot_size + max(10, height // 100), status_y),
                  "CONNEXION EN COURS", font=status_font, fill=MUTED)
        return self._write_image(image)

    def start_progress(self):
        """Animate a small framebuffer-only bar while network startup waits."""
        if self.progress_rect is None or self._progress_thread is not None:
            return
        self._progress_stop = threading.Event()
        self._progress_thread = threading.Thread(
            target=self._animate_progress, name="wignet-startup-progress", daemon=True
        )
        self._progress_thread.start()

    def _animate_progress(self):
        rect = self.progress_rect
        segment_w = max(24, rect.width // 5)
        travel = max(0, rect.width - segment_w)
        started = time.monotonic()
        while not self._progress_stop.is_set():
            phase = ((time.monotonic() - started) % 2.4) / 2.4
            offset = int(travel * phase)
            row = progress_row(rect.width, segment_w, offset)
            try:
                for y in range(rect.height):
                    written = os.pwrite(self.fd, row, (rect.y + y) * self.stride + rect.x * 2)
                    if written != len(row):
                        raise OSError(f"short progress-bar write: {written} of {len(row)} bytes")
            except OSError as exc:
                print(f"startup progress animation stopped ({exc})", file=sys.stderr)
                return
            if self._progress_stop.wait(0.08):
                return

    def stop_progress(self):
        if self._progress_stop is None:
            return
        self._progress_stop.set()
        if self._progress_thread is not None:
            self._progress_thread.join(timeout=1)
        self._progress_thread = None
        self._progress_stop = None

    def page(self, data):
        self.stop_progress()
        # The server rendered exactly the size declared by this client.  Do
        # not resize or otherwise transform its content locally.
        image = Image.open(io.BytesIO(data), formats=("PNG",))
        if image.size != self.size:
            raise ValueError(f"server returned {image.size}, expected {self.size}")
        self._write_image(image)
        self.has_page = True

    @staticmethod
    def wait(seconds):
        time.sleep(seconds)


def poll_once(settings, display, etag, state_dir, first_frame=False,
              event_waker=None, player=None):
    """Fetch the server-selected frame, then ask when to poll it again."""
    try:
        # The splash replaced whatever was on screen before restart.  Do not
        # accept a 304 until this process has actually drawn a full frame.
        data, received_etag, unchanged, wake_event = get_image(
            settings, None if first_frame else etag)
        if data is not None:
            display.page(data)
            etag = received_etag
            save_etag(state_dir, etag)
        elif not unchanged:
            raise ValueError("empty image response")
        if event_waker is not None:
            event_waker.update(wake_event)
    except (HTTPError, URLError, ValueError, OSError) as exc:  # OSError: undecodable PNG
        print(f"image unavailable ({exc}); keeping the current page", file=sys.stderr)
    # An event boundary can change the selected image without any local logic.
    # The image comes first, so a pushed text is on screen before its sound.
    return etag, next_seconds(settings, player)


def main(argv=None):
    host, mac, width, height, state_dir, once = parse_args(argv)
    sys.stderr = SupportLog(sys.stderr, Path(state_dir) / SUPPORT_LOG)
    # The CEC and TV-power modules report through `logging`: what was sent to
    # the TV, or why nothing was.  syslog (`logger -t wignet-tv`) adds the time.
    logging.basicConfig(level=logging.INFO, format="%(message)s", stream=sys.stderr)
    # Wake the television before starting Pygame or touching the network.  CEC
    # is optional and cannot prevent the local splash or Wignet page from
    # being displayed.
    cec = None
    try:
        cec = CEC.from_environment()
    except ValueError as exc:
        print(f"CEC disabled ({exc})", file=sys.stderr)
        cec = CEC(False)
    cec.wake_tv()
    # Events and sounds wake the TV through `power`, which also puts back to
    # sleep a TV it woke itself, and leaves a TV someone switched on alone.
    power = TVPower(cec)
    event_waker = EventWakeScheduler(power)
    display = Display()
    splash_frame = display.splash()
    display.start_progress()
    save_splash(state_dir, display, splash_frame)
    if width is None:
        width, height = display.size
    player = None if once else start_player(state_dir, power)
    settings = Settings(host, mac, *checked_size(width, height),
                        caps=SOUND_CAPS if player is not None else CAPS,
                        fw=installed_version())
    # Declare first: until the server has read caps=, the PNG answers 406.
    # Every normal cycle below keeps the common Wignet order: image, schedule,
    # wait.  The server then chooses the image for any active event or slide.
    next_seconds(settings, player)
    etag = load_etag(state_dir)
    while True:
        etag, delay = poll_once(settings, display, etag, state_dir,
                                first_frame=not display.has_page,
                                event_waker=event_waker, player=player)
        if once:
            return 0
        display.wait(delay)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(0)
