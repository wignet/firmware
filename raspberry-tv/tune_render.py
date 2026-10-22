"""Synthesize an MML v1 tune into 16-bit PCM, as the Wignet frames play it.

The reference is `render()` in agent_skill/skills/wignet-tunes/scripts/tune.py,
itself sample-identical to the PhotoPainter's synthesizer (mml_tune.cpp). It
walks every sample in Python: minutes for one loop on a Pi Zero. This port
computes each note at once with numpy and gives the same samples
(test_tune_render.py compares them), so a pushed chime is ready in time.

Tunes are parsed by the platform's own parser, `mml.py` (backoffice/mml.py,
installed next to this file), never by a second one.
"""
import functools

import numpy as np

import mml

RATE = 48000
CHANNEL_SPAN = 16000  # peak to peak at v15, as in mml_tune.cpp
_DUTY_EIGHTHS = (1, 2, 4, 6)
_WRAP = 1 << 32


def _tdiv(values, divisor):
    """C integer division, which truncates toward zero."""
    quotient = np.abs(values) // divisor
    return np.where(values >= 0, quotient, -quotient)


@functools.lru_cache(maxsize=2)
def _lfsr_states(timbre):
    """Noise register states from 0x7FFF, as (prefix length, cycle length, states).

    The noise channel steps its register at each phase wrap. The sequence is
    periodic after a short prefix (16-bit state), so it is walked once.
    """
    seen, states, lfsr = {}, [], 0x7FFF
    while lfsr not in seen:
        seen[lfsr] = len(states)
        states.append(lfsr)
        bit = (lfsr ^ (lfsr >> 1)) & 1
        lfsr = (lfsr >> 1) | (bit << 14)
        if timbre == 1:
            lfsr = (lfsr & ~(1 << 6)) | (bit << 6)
        lfsr &= 0xFFFF
    start = seen[lfsr]
    return start, len(states) - start, np.array(states, dtype=np.int64)


def _note_samples(index, note, count, rate):
    """The `count` audible samples of one note on channel `index`, before mixing."""
    frequency = 440.0 * pow(2.0, (note.note - 69) / 12.0)
    if index == 3:
        frequency = min(frequency * 8.0, rate * 0.99)
    increment = int(frequency * 4294967296.0 / rate)
    k = np.arange(count, dtype=np.int64)
    # k < 180 s * 48 kHz and increment < 2**32: the product fits in int64.
    advanced = k * increment
    phase = advanced & (_WRAP - 1)
    if index < 2:
        duty = _DUTY_EIGHTHS[note.timbre]
        value = np.where(phase < (duty << 29), CHANNEL_SPAN * (8 - duty) // 8,
                         -(CHANNEL_SPAN * duty // 8))
    elif index == 2:
        t = phase >> 16
        if note.timbre == 0:
            value = _tdiv(np.where(t < 32768, t - 16384, 49151 - t) * CHANNEL_SPAN, 32768)
        else:
            value = _tdiv((t - 32768) * CHANNEL_SPAN, 65536)
    else:
        start, cycle, states = _lfsr_states(note.timbre)
        wraps = advanced >> 32
        position = np.where(wraps < start, wraps, start + (wraps - start) % cycle)
        value = np.where(states[position] & 1, -CHANNEL_SPAN // 2, CHANNEL_SPAN // 2)
    level = np.full(count, note.volume, dtype=np.int64)
    if note.envelope:
        # The level drops by one every `step` samples, the first drop counted
        # from the note's first sample, and stays at 0.
        step = max(1, note.envelope * rate // 64)
        level = np.maximum(0, level - (k + 1) // step)
    return _tdiv(value * level, 15)


def render(tune, rate=RATE):
    """One loop of `tune` (an mml.Tune) as mono int16 samples."""
    ticks_per_minute = tune.tempo * mml.TICKS_PER_QUARTER

    def sample_at(tick):
        return tick * rate * 60 // ticks_per_minute

    total = sample_at(tune.length_ticks)
    mix = np.zeros(total, dtype=np.int64)
    for index, channel in enumerate(mml.CHANNELS):
        start = 0
        for note in tune.channels.get(channel, ()):
            # A note takes over when the previous one ends.
            end = sample_at(note.tick + note.length)
            begin, start = start, max(start, end)
            if note.note < 0 or end <= begin:
                continue
            audible = min(sample_at(note.tick + note.gate), end, total) - begin
            if audible > 0:
                mix[begin:begin + audible] += _note_samples(index, note, audible, rate)
    return np.clip(mix, -32768, 32767).astype("<i2")


def render_text(text, rate=RATE):
    """Parse an MML text with the platform's parser, then render one loop."""
    return render(mml.parse(text), rate)


def fallback_tone(seconds=1.0, rate=RATE):
    """The firmware's tone without a tune or a WAV: 880 Hz pulse, 250 ms on/off.

    Full scale is the firmware's level 12000; the player applies the volume.
    """
    sample = np.arange(int(seconds * rate), dtype=np.int64)
    audible = (sample // (rate // 4)) % 2 == 0
    high = (sample % (rate // 880)) < (rate // 1760)
    return np.where(audible & high, 12000, 0).astype("<i2")
