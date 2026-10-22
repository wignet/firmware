"""Ring the Wignet's sounds through the TV's speakers, over HDMI.

The poll loop hands every /schedule answer to `Player.update`; a thread of
its own rings each held sound at its time, one at a time, while the display
keeps updating. The TV is woken through CEC a few seconds ahead, since a TV in
standby plays nothing. Audio goes to ALSA through `aplay` as 48 kHz mono
16-bit PCM, the frames' format.

Like the PhotoPainter (audio_hal.cpp): a tune loops for `fadeInSec +
maxDurationSec`, rising from silence to `volume` over the fade-in with the
square of the progress; without a tune, the WAV named by `sound` if this TV
has one, else an 880 Hz beep.
"""
import subprocess
import sys
import threading
import time
import wave
from pathlib import Path

import numpy as np

import mml
import sound_list
import tune_render

RATE = tune_render.RATE
SOUNDS_DIR = Path(__file__).with_name("sounds")
# Written to aplay at a time; aplay's own buffer paces the writes.
CHUNK = RATE // 10
IDLE_SECONDS = 60


def log(message):
    print(f"sound: {message}", file=sys.stderr, flush=True)


def gain(positions, volume, fade_samples):
    """Per-sample gain at `positions` (samples since the start), as audio_hal.cpp."""
    full = volume / 100.0
    if fade_samples <= 0:
        return np.full(len(positions), full)
    progress = np.minimum(positions / fade_samples, 1.0)
    return full * progress * progress


def read_wav(path):
    """Samples of a 48 kHz mono 16-bit WAV, or None (the frames' only format)."""
    try:
        with wave.open(str(path), "rb") as source:
            if (source.getframerate(), source.getnchannels(), source.getsampwidth()) != (RATE, 1, 2):
                log(f"{path.name} must be 48 kHz mono 16-bit PCM; using the beep")
                return None
            data = source.readframes(source.getnframes())
    except (OSError, EOFError, wave.Error):
        return None
    samples = np.frombuffer(data, dtype="<i2")
    return samples if len(samples) else None


class Player:
    def __init__(self, state_dir, power, device="default", popen=subprocess.Popen):
        self.store = sound_list.Store(state_dir)
        self.cache = Path(state_dir) / "tunes"
        self.power = power
        self.device = device
        self.popen = popen
        self._lock = threading.Lock()
        self._render_lock = threading.Lock()
        self._changed = threading.Event()
        # (serverEpoch, monotonic time it was read): a Pi Zero has no RTC and
        # may run before NTP, so the server's clock wins once it has answered.
        self._server_clock = None
        self._prepared = {}

    def now(self):
        if self._server_clock is None:
            return int(time.time())
        epoch, read_at = self._server_clock
        return int(epoch + time.monotonic() - read_at)

    def update(self, payload):
        """Take a /schedule answer: hold its sounds and prepare their tunes."""
        epoch = payload.get("serverEpoch") if isinstance(payload, dict) else None
        if type(epoch) is int and epoch > 0:
            self._server_clock = (epoch, time.monotonic())
        answer = sound_list.parse_answer(payload)
        if answer is None:
            return
        with self._lock:
            changed = self.store.update(*answer, self.now())
            tunes = dict(self.store.tunes)
        if changed:
            self._changed.set()
            threading.Thread(target=self._prepare, args=(tunes,), name="wignet-tune-render",
                             daemon=True).start()

    def start(self):
        threading.Thread(target=self._run, name="wignet-sound", daemon=True).start()

    # -- tunes ---------------------------------------------------------------

    def _prepare(self, tunes):
        """Render every held tune ahead of its time, and forget the others."""
        try:
            for path in self.cache.glob("*.pcm"):
                if path.stem not in tunes:
                    path.unlink(missing_ok=True)
        except OSError:
            pass
        for tune_id, text in tunes.items():
            self._tune(tune_id, text)

    def _tune(self, tune_id, text):
        """One loop of a held tune, from the cache or rendered now; None if refused."""
        path = self.cache / f"{tune_id}.pcm"
        with self._render_lock:
            try:
                samples = np.fromfile(path, dtype="<i2")
                if len(samples):
                    return samples
            except (OSError, ValueError):
                pass
            started = time.monotonic()
            try:
                samples = tune_render.render_text(text)
            except mml.MmlError as exc:
                log(f"tune {tune_id} refused ({exc})")
                return None
            log(f"tune {tune_id} rendered in {time.monotonic() - started:.1f} s "
                f"({len(samples) / RATE:.1f} s per loop)")
            try:
                self.cache.mkdir(parents=True, exist_ok=True)
                temporary = path.with_suffix(".tmp")
                samples.tofile(temporary)
                temporary.replace(path)
            except OSError as exc:
                log(f"cannot cache tune {tune_id} ({exc})")
            return samples

    def samples_for(self, entry, tunes):
        """What an entry plays, looped: its tune, else its WAV, else the beep."""
        text = tunes.get(entry["tuneId"]) if entry["tuneId"] else None
        if text:
            samples = self._tune(entry["tuneId"], text)
            if samples is not None:
                return samples
        if entry["sound"]:
            samples = read_wav(SOUNDS_DIR / f"{entry['sound']}.wav")
            if samples is not None:
                return samples
        return tune_render.fallback_tone()

    # -- ringing -------------------------------------------------------------

    def _prepare_tv(self, item):
        """Request this sound's hold once; read the TV's shared readiness."""
        occurrence_id = item["occurrenceId"]
        if occurrence_id not in self._prepared:
            self._prepared = {key: value for key, value in self._prepared.items()
                              if time.monotonic() - value < 3600}
            hold = self.power.wake_lead + item["fadeInSec"] + item["maxDurationSec"]
            self.power.wake_tv(hold_seconds=hold)
            self._prepared[occurrence_id] = time.monotonic()
        return self.power.ready_in()

    def step(self):
        """Ring what is due, or wake the TV for what comes; seconds to wait next."""
        with self._lock:
            now = self.now()
            entry = sound_list.due(self.store.entries, self.store.done, now)
            upcoming = sound_list.next_epoch(self.store.entries, self.store.done, now)
            tunes = dict(self.store.tunes)
            if entry is not None:
                coming = entry
            else:
                coming = next((item for item in self.store.entries
                               if item["epoch"] == upcoming
                               and item["occurrenceId"] not in self.store.done), None)
        if entry is not None:
            delay = self._prepare_tv(entry)
            # Image and sound events share the same TV: the image path may
            # have sent the wake just before this sound reached /schedule.
            if delay > 0:
                return delay
            with self._lock:
                self.store.mark_done(entry["occurrenceId"])
            self.ring(entry, tunes)
            return 0
        if upcoming is None:
            return IDLE_SECONDS
        if upcoming - now <= self.power.wake_lead and coming is not None:
            self._prepare_tv(coming)
            return max(0.2, upcoming - now)
        return min(IDLE_SECONDS, upcoming - now - self.power.wake_lead)

    def _run(self):
        while True:
            try:
                delay = self.step()
            except Exception as exc:  # the sound thread must outlive one bad sound
                log(f"unexpected error ({exc!r})")
                delay = IDLE_SECONDS
            if delay > 0 and self._changed.wait(delay):
                self._changed.clear()

    def ring(self, entry, tunes):
        samples = self.samples_for(entry, tunes)
        total = (entry["fadeInSec"] + entry["maxDurationSec"]) * RATE
        late = self.now() - entry["epoch"]
        log(f"ringing {'pushed sound ' if entry['pushed'] else ''}{entry['occurrenceId']} "
            f"({late} s after its time), volume {entry['volume']}, "
            f"fade-in {entry['fadeInSec']} s, then {entry['maxDurationSec']} s")
        command = ["aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", str(RATE), "-c", "1",
                   "-D", self.device]
        try:
            process = self.popen(command, stdin=subprocess.PIPE)
        except OSError as exc:
            log(f"cannot start aplay ({exc}); install alsa-utils")
            return False
        fade = entry["fadeInSec"] * RATE
        try:
            for start in range(0, total, CHUNK):
                positions = np.arange(start, min(start + CHUNK, total))
                chunk = samples[positions % len(samples)] * gain(positions, entry["volume"], fade)
                process.stdin.write(np.round(chunk).astype("<i2").tobytes())
            process.stdin.close()
            process.wait()
        except (OSError, ValueError) as exc:
            log(f"playback stopped ({exc})")
            process.kill()
            return False
        if process.returncode:
            log(f"aplay failed with status {process.returncode} (device {self.device})")
            return False
        return True
