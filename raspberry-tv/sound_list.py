"""The upcoming sounds the TV holds, from /schedule `sounds` (sound_list1).

The same rules as the PhotoPainter's esp32/src/sound_list.cpp: what
is due, what comes next, how a new answer replaces the held list, and the
set of occurrences already rung. Storing them lives here too, in the state
directory, so a restart neither forgets a sound nor rings one twice.
"""
import json
import re
import sys
from pathlib import Path

# A sound found later than this after its time is skipped, not rung late.
GRACE_SECONDS = 120
# The server sends at most 16; pushed sounds carried over can add a few.
MAX_ENTRIES = 20
# Occurrences already rung: more than a list holds, so none drops out while listed.
DONE_KEPT = 32
_NAME = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
_TUNE_ID = re.compile(r"^[0-9a-f]{1,40}$")


def _bounded(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError
    return value


def parse_entry(raw):
    """One validated `sounds` entry, or None for a malformed one."""
    try:
        entry = {
            "occurrenceId": raw["occurrenceId"],
            "epoch": _bounded(raw["epoch"], 1, 2 ** 40),
            "pushed": raw.get("pushed") is True,
            "sound": raw.get("sound") or "",
            "volume": _bounded(raw.get("volume", 50), 0, 100),
            "fadeInSec": _bounded(raw.get("fadeInSec", 0), 0, 3600),
            "maxDurationSec": _bounded(raw.get("maxDurationSec", 30), 1, 3600),
            "tuneId": raw.get("tuneId") or "",
        }
    except (KeyError, TypeError, AttributeError, ValueError):
        return None
    if not isinstance(entry["occurrenceId"], str) or not 0 < len(entry["occurrenceId"]) <= 128:
        return None
    if entry["sound"] and not (isinstance(entry["sound"], str) and _NAME.fullmatch(entry["sound"])):
        entry["sound"] = ""
    if entry["tuneId"] and not (isinstance(entry["tuneId"], str) and _TUNE_ID.fullmatch(entry["tuneId"])):
        entry["tuneId"] = ""
    return entry


def parse_answer(payload):
    """(entries, tunes) from a /schedule answer, or None when it carries no list.

    A server that sends no `sounds` (an older one) leaves the held list as is.
    """
    if not isinstance(payload, dict) or not isinstance(payload.get("sounds"), list):
        return None
    entries = [entry for entry in map(parse_entry, payload["sounds"]) if entry is not None]
    tunes = payload.get("tunes")
    tunes = {key: text for key, text in tunes.items()
             if isinstance(key, str) and _TUNE_ID.fullmatch(key) and isinstance(text, str)} \
        if isinstance(tunes, dict) else {}
    return entries, tunes


def mark_done(done, occurrence_id):
    """Record `occurrence_id` as rung, keeping the newest DONE_KEPT."""
    if not occurrence_id or occurrence_id in done:
        return done
    return (done + [occurrence_id])[-DONE_KEPT:]


def merge(held, incoming, done, now):
    """The list to hold after an answer.

    The server is the authority on planned sounds, so `incoming` replaces
    them. A pushed sound is sent once, so one held but not rung yet is kept
    while it can still ring, unless `incoming` lists it again.
    """
    listed = {entry["occurrenceId"] for entry in incoming}
    result = [entry for entry in held
              if entry["pushed"] and entry["occurrenceId"] not in done
              and now <= entry["epoch"] + GRACE_SECONDS and entry["occurrenceId"] not in listed]
    for entry in incoming:
        if len(result) >= MAX_ENTRIES:
            break
        if entry["occurrenceId"] not in {kept["occurrenceId"] for kept in result}:
            result.append(entry)
    return result


def due(entries, done, now):
    """The sound to ring now: due, within its grace, not rung yet, earliest first."""
    best = None
    for entry in entries:
        if not entry["epoch"] <= now <= entry["epoch"] + GRACE_SECONDS:
            continue
        if entry["occurrenceId"] in done:
            continue
        if best is None or entry["epoch"] < best["epoch"]:
            best = entry
    return best


def next_epoch(entries, done, now):
    """Time of the next sound still to ring after `now`, or None."""
    upcoming = [entry["epoch"] for entry in entries
                if entry["epoch"] > now and entry["occurrenceId"] not in done]
    return min(upcoming, default=None)


class Store:
    """The held list, its tunes and the rung occurrences, in the state directory."""

    def __init__(self, state_dir):
        self.directory = Path(state_dir)
        self.entries, self.tunes = self._read("sounds.json", ([], {}),
                                              lambda v: ([e for e in map(parse_entry, v["entries"]) if e],
                                                         dict(v["tunes"])))
        self.done = self._read("rung.json", [],
                               lambda v: [str(item) for item in v][-DONE_KEPT:])

    def _read(self, name, default, convert):
        try:
            return convert(json.loads((self.directory / name).read_text(encoding="utf-8")))
        except (OSError, ValueError, TypeError, KeyError, AttributeError):
            return default

    def _write(self, name, value):
        try:
            self.directory.mkdir(parents=True, exist_ok=True)
            temporary = self.directory / (name + ".tmp")
            temporary.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
            temporary.replace(self.directory / name)
        except OSError as exc:
            print(f"cannot persist {name} ({exc}); keeping it in memory", file=sys.stderr)

    def update(self, entries, tunes, now):
        """Hold a new answer; True when what is held changed (written only then)."""
        merged = merge(self.entries, entries, self.done, now)
        kept_ids = {entry["tuneId"] for entry in merged if entry["tuneId"]}
        # A pushed sound carried over keeps its tune though the answer omits it.
        tunes = {key: text for key, text in {**self.tunes, **tunes}.items() if key in kept_ids}
        if merged == self.entries and tunes == self.tunes:
            return False
        self.entries, self.tunes = merged, tunes
        self._write("sounds.json", {"entries": merged, "tunes": tunes})
        return True

    def mark_done(self, occurrence_id):
        """Persist before playback, so a restart or a crash never replays it."""
        self.done = mark_done(self.done, occurrence_id)
        self._write("rung.json", self.done)
