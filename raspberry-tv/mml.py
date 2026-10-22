"""MML v1: the text tune format alarm-capable frames play (see docs/mml.md).

The platform's own parser, which validates every tune before sending it on
/schedule; the TV client parses with this copy of it. The firmware parser
(esp32/src/mml_tune.cpp) walks the same grammar, and the platform tests both
over the same fixtures, so a tune plays the same everywhere.

Four Game Boy-style channels: A and B are pulse waves, C a wave channel,
D noise. Time is counted in ticks, 96 per quarter note.
"""
import re
from dataclasses import dataclass

HEADER = "#MML1"
MAX_BYTES = 2048
MAX_SECONDS = 180
TICKS_PER_QUARTER = 96
TICKS_PER_WHOLE = TICKS_PER_QUARTER * 4
# Upper bound on any valid tune's length, whatever its tempo; walking stops
# here so a loop cannot make validation run away.
MAX_TICKS = MAX_SECONDS * 240 * TICKS_PER_QUARTER // 60
# Commands executed per channel, loops expanded. Bounds the work of loops
# that take no time, like [[[[v1]16]16]16]16. Same limit in the firmware.
MAX_STEPS = 50000
LENGTHS = (1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64)
CHANNELS = "ABCD"
MIN_TEMPO, MAX_TEMPO, DEFAULT_TEMPO = 40, 240, 120
MIN_NOTE, MAX_NOTE = 24, 107  # c in octave 1 .. b in octave 7
MAX_LOOP_DEPTH = 4
# Timbre (@) range per channel: pulse duty 12.5/25/50/75 %, wave
# triangle/sawtooth, noise white/metallic.
TIMBRES = {"A": 3, "B": 3, "C": 1, "D": 1}
DEFAULT_TIMBRE = {"A": 2, "B": 2, "C": 0, "D": 0}
DEFAULT_VOLUME = 12
DEFAULT_GATE = 7
_SEMITONES = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
# What a normalized tune may contain after its header. The frame's JSON reader cuts the alarm
# object at `}` and cannot be trusted with `"` or `\`, so none may reach it.
_NORMALIZED_CHARS = re.compile(r"^[A-Da-z0-9 \n#+\-.&<>\[\]@|]*$")


class MmlError(ValueError):
    def __init__(self, message, line=None, near=None):
        where = ""
        if line is not None:
            where = f"line {line}"
            if near:
                where += f' near "{near}"'
            where += ": "
        super().__init__(where + message)
        self.line = line


@dataclass(frozen=True)
class Note:
    """One note or rest. `note` is a MIDI number, or -1 for a rest."""
    tick: int
    length: int
    gate: int
    note: int
    volume: int
    timbre: int
    envelope: int
    line: int = 0  # input line it came from; not part of what the frame plays

    def as_tuple(self):
        return (self.tick, self.length, self.gate, self.note, self.volume,
                self.timbre, self.envelope)


@dataclass(frozen=True)
class Tune:
    text: str  # normalized
    tempo: int
    length_ticks: int
    channels: dict  # channel letter -> [Note], in A..D order

    @property
    def seconds(self):
        return self.length_ticks * 60 / (self.tempo * TICKS_PER_QUARTER)


def _beats(ticks):
    return f"{ticks / TICKS_PER_QUARTER:g}"


def _split(text):
    """Return [(input line number, channel, body)] with bodies normalized."""
    if not isinstance(text, str):
        raise MmlError("tune must be a string")
    lines = []
    header_allowed = True
    for number, raw in enumerate(text.replace("\r", "").split("\n"), start=1):
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith("#"):
            if line == HEADER and header_allowed:
                header_allowed = False
                continue
            raise MmlError(f"only a first line {HEADER} may start with #", number)
        header_allowed = False
        channel, body = line[0], line[1:]
        if channel not in CHANNELS or (body and not body[0].isspace()):
            raise MmlError("a line must start with a channel letter A, B, C or D "
                           "and a space", number, line[:12])
        body = " ".join(body.split()).lower()
        if body:
            lines.append((number, channel, body))
    return lines


def normalize(text):
    """Return the canonical form: header, then one `X commands` line per input line.

    Comments go, whitespace collapses, commands are lowercased. Parsing the
    result gives exactly the same notes as parsing the input.
    """
    return "\n".join([HEADER] + [f"{ch} {body}" for _, ch, body in _split(text)])


class _Cursor:
    """Walks one channel's commands and yields its notes, expanding loops.

    Mirrors Mml::ChannelCursor in the firmware; keep the two in step.
    """

    def __init__(self, lines, channel):
        self.lines = [(n, body) for n, ch, body in lines if ch == channel]
        self.channel = channel
        self.octave = 4
        self.default_length = TICKS_PER_QUARTER
        self.volume = DEFAULT_VOLUME
        self.timbre = DEFAULT_TIMBRE[channel]
        self.envelope = 0
        self.gate = DEFAULT_GATE
        self.tempo = None
        self.tick = 0
        self.steps = 0

    def _error(self, message, line, body, pos):
        raise MmlError(message, line, body[max(0, pos - 4):pos + 8])

    @staticmethod
    def _number(body, pos):
        end = pos
        while end < len(body) and body[end].isdigit():
            end += 1
        return (int(body[pos:end]) if end > pos else None), end

    def _length(self, line, body, pos):
        start = pos
        number, pos = self._number(body, pos)
        if number is None:
            base = self.default_length
        else:
            if number not in LENGTHS:
                self._error(f"length {number} is not one of "
                            f"{', '.join(map(str, LENGTHS))}", line, body, start)
            base = TICKS_PER_WHOLE // number
        total, add = base, base
        while pos < len(body) and body[pos] == ".":
            if add % 2:
                self._error("this dotted length is too short", line, body, pos)
            add //= 2
            total += add
            pos += 1
        return total, pos

    def _pitch(self, line, body, pos):
        start = pos
        semitone = _SEMITONES[body[pos]]
        pos += 1
        if pos < len(body) and body[pos] in "+#":
            semitone += 1
            pos += 1
        elif pos < len(body) and body[pos] == "-":
            semitone -= 1
            pos += 1
        note = 12 * (self.octave + 1) + semitone
        if not MIN_NOTE <= note <= MAX_NOTE:
            self._error("note is outside the playable range (c in o1 to b in o7)",
                        line, body, start)
        return note, pos

    @staticmethod
    def _skip_spaces(body, pos):
        while pos < len(body) and body[pos] in " |":
            pos += 1
        return pos

    def notes(self):
        for line, body in self.lines:
            yield from self._line_notes(line, body)

    def _line_notes(self, line, body):
        pos = 0
        loops = []  # [position after "[", repeats left or None before the first "]"]
        while True:
            pos = self._skip_spaces(body, pos)
            if pos >= len(body):
                break
            self.steps += 1
            if self.steps > MAX_STEPS:
                self._error("tune repeats too many commands; use fewer or shorter loops",
                            line, body, pos)
            c = body[pos]
            start = pos
            if c in _SEMITONES or c == "r":
                if c == "r":
                    note, pos = -1, pos + 1
                else:
                    note, pos = self._pitch(line, body, pos)
                length, pos = self._length(line, body, pos)
                while note >= 0:
                    after = self._skip_spaces(body, pos)
                    if after >= len(body) or body[after] != "&":
                        break
                    nxt = self._skip_spaces(body, after + 1)
                    if nxt >= len(body) or body[nxt] not in _SEMITONES:
                        self._error("& must be followed by a note of the same pitch",
                                    line, body, after)
                    tied, pos = self._pitch(line, body, nxt)
                    if tied != note:
                        self._error("& must join two notes of the same pitch",
                                    line, body, nxt)
                    extra, pos = self._length(line, body, pos)
                    length += extra
                gate = length * self.gate // 8 if note >= 0 else 0
                yield Note(self.tick, length, gate, note, self.volume,
                           self.timbre, self.envelope, line)
                self.tick += length
                if self.tick > MAX_TICKS:
                    self._error(f"tune is longer than {MAX_SECONDS} s", line, body, start)
            elif c == "l":
                if self._number(body, pos + 1)[0] is None:
                    self._error("l needs a length, like l8", line, body, start)
                self.default_length, pos = self._length(line, body, pos + 1)
            elif c in "otvsq@":
                value, pos = self._number(body, pos + 1)
                if value is None:
                    self._error(f"{c} needs a number", line, body, start)
                if c == "o":
                    if not 1 <= value <= 7:
                        self._error("octave must be between 1 and 7", line, body, start)
                    self.octave = value
                elif c == "t":
                    if not MIN_TEMPO <= value <= MAX_TEMPO:
                        self._error(f"tempo must be between {MIN_TEMPO} and {MAX_TEMPO}",
                                    line, body, start)
                    if self.tick:
                        self._error("tempo must come before the channel's first note",
                                    line, body, start)
                    self.tempo = value
                elif c == "v":
                    if not 0 <= value <= 15:
                        self._error("volume must be between 0 and 15", line, body, start)
                    self.volume = value
                elif c == "s":
                    if not 0 <= value <= 7:
                        self._error("envelope decay s must be between 0 and 7",
                                    line, body, start)
                    self.envelope = value
                elif c == "q":
                    if not 1 <= value <= 8:
                        self._error("gate q must be between 1 and 8", line, body, start)
                    self.gate = value
                else:
                    top = TIMBRES[self.channel]
                    if not 0 <= value <= top:
                        self._error(f"@ must be between 0 and {top} on channel "
                                    f"{self.channel}", line, body, start)
                    self.timbre = value
            elif c in "<>":
                self.octave += 1 if c == ">" else -1
                if not 1 <= self.octave <= 7:
                    self._error("octave must stay between 1 and 7", line, body, start)
                pos += 1
            elif c == "[":
                if len(loops) >= MAX_LOOP_DEPTH:
                    self._error(f"loops nest at most {MAX_LOOP_DEPTH} deep",
                                line, body, start)
                pos += 1
                loops.append([pos, None])
            elif c == "]":
                if not loops:
                    self._error("] without a matching [ on this line", line, body, start)
                count, pos = self._number(body, pos + 1)
                count = 2 if count is None else count
                if not 2 <= count <= 16:
                    self._error("a loop repeats between 2 and 16 times", line, body, start)
                frame = loops[-1]
                if frame[1] is None:
                    frame[1] = count - 1
                if frame[1] > 0:
                    frame[1] -= 1
                    pos = frame[0]
                else:
                    loops.pop()
            elif c == "&":
                self._error("& must follow a note", line, body, start)
            else:
                self._error(f"unknown command {c!r}", line, body, start)
        if loops:
            self._error("[ is never closed on this line", line, body, loops[-1][0] - 1)


def parse(text):
    """Validate and normalize a tune; raise MmlError naming the input line."""
    lines = _split(text)
    normalized = "\n".join([HEADER] + [f"{ch} {body}" for _, ch, body in lines])
    size = len(normalized.encode())
    if size > MAX_BYTES:
        raise MmlError(f"tune is {size} bytes once normalized; the limit is {MAX_BYTES}")
    if not lines:
        raise MmlError("tune has no channel lines; start a line with A, B, C or D")

    used = [ch for ch in CHANNELS if any(c == ch for _, c, _ in lines)]
    channels, tempos, lengths = {}, {}, {}
    for channel in used:
        cursor = _Cursor(lines, channel)
        channels[channel] = list(cursor.notes())
        lengths[channel] = cursor.tick
        if cursor.tempo is not None:
            tempos[channel] = cursor.tempo
    # Every character the cursor accepted is in this set; checked anyway
    # because the firmware's JSON reader depends on it.
    if not _NORMALIZED_CHARS.fullmatch(normalized[len(HEADER):]):
        raise MmlError("tune contains a character that is not allowed")
    if len(set(tempos.values())) > 1:
        detail = ", ".join(f"{ch} t{t}" for ch, t in tempos.items())
        raise MmlError(f"every channel must use the same tempo ({detail})")
    tempo = next(iter(tempos.values()), DEFAULT_TEMPO)

    longest = max(used, key=lambda ch: lengths[ch])
    for channel in used:
        if lengths[channel] != lengths[longest]:
            raise MmlError(
                f"channel {channel} lasts {_beats(lengths[channel])} beats but channel "
                f"{longest} lasts {_beats(lengths[longest])}; every channel must last "
                "the same (pad the shorter one with rests)"
            )
    if not any(n.note >= 0 for notes in channels.values() for n in notes):
        raise MmlError("tune has no notes")
    tune = Tune(normalized, tempo, lengths[longest], channels)
    if tune.seconds > MAX_SECONDS:
        raise MmlError(f"tune lasts {tune.seconds:.0f} s; the limit is {MAX_SECONDS} s "
                       "(it loops for the whole alarm anyway)")
    return tune


def dump_events(tune):
    """Listing both parsers print, compared line for line in the tests."""
    out = [f"tempo {tune.tempo} length {tune.length_ticks}"]
    for channel, notes in tune.channels.items():
        for note in notes:
            out.append(channel + " " + " ".join(str(v) for v in note.as_tuple()))
    return "\n".join(out) + "\n"
