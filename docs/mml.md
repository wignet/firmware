# MML v1 alarm tunes

An alarm can carry a **tune**: a short text in Music Macro Language that the
frame plays on four Game Boy-style channels, looped for the whole alarm, in
place of the WAV named by `sound`. Agents write it in an event's `sound`
through `set_events` or `update_event` (MCP and REST); the platform validates and normalizes it,
stores it in the event's sound (`device_events.sound_json`, see
`docs/events.md`), and sends it on `/schedule` to firmware that
reported `tune_mml1`. The frame keeps it in NVS next to the rest of the cached
alarm, so it rings offline like any other alarm.

This file is the dialect's contract. Two parsers implement it and must agree:

| | Role |
| --- | --- |
| `backoffice/mml.py` | Reference: validates, normalizes, reports errors to agents |
| `esp32/src/mml_tune.cpp` | Firmware: validates again, walks notes while playing, synthesizes |

Two more pieces mirror the synthesizer, and must stay sample-identical to
the firmware:
- `tune.py wav` in the `wignet-tunes` agent skill
  (`agent_skill/skills/wignet-tunes/`) renders a WAV preview in a sandbox;
- `backoffice/tune_player/synth.js`, a JavaScript port of the parser and
  synth, plays the tune in the browser at `/w/tune#<base64url of the tune>`
  (the page never sees the fragment, so the platform stores nothing). The
  `preview_tune` MCP tool checks a tune and returns that link, and serves
  the same page as an MCP App (`ui://wignet/tune-player`) that hosts
  supporting SEP-1865 render in the conversation. `tests/test_tune_player.py`
  runs the port under Node against the Python parser and the skill's
  renderer: same verdicts, same errors, same samples.

`tests/test_mml.py` builds the firmware parser on the host (any `c++`; on
macOS the Command Line Tools clang is tried too) and checks that both accept
and refuse the same tunes and produce the same notes: the fixtures in
`tests/fixtures/mml/`, then 600 generated tunes. It also compares the
preview's samples with the firmware's, byte for byte. Change the grammar in both
parsers, this file, the `tune` description agents read
(`TUNE_DESCRIPTION` in `backoffice/owner_openapi.py`) and the skill's
`SKILL.md.in` together; a synthesis change also goes into `tune.py`. An
incompatible change is a new dialect: `#MML2` and a new `tune_mml2`
capability, so frames in the field never receive a tune they would misplay.

## Example

```
#MML1
; Four-channel wake-up jingle
A t140 o5 l8 v13 @1 c e g >c< | g e c r
B o4 l4 v9 @2 c e g4 [c8]2
C o3 l2 c g
D l8 s2 [c r]4
```

## Tunes picked by name

An event can name a tune instead of carrying it: `"sound": {"tune_name":
"first_light"}`. `backoffice/tune_library.py` looks the name up when the
event is saved and copies the normalized MML into `sound.tune`, keeping the
name beside it as a label. `/schedule` and the firmware only ever see MML, so
nothing changes on the wire, and editing or deleting a library tune never
changes an alarm already set (set the name again to take the new version).

| Source | Where | Who sees it |
| --- | --- | --- |
| Built-in | `backoffice/tunes/<name>.mml`, listed in `tune_library.BUILT_IN` | Every Wignet |
| Saved | `device_tunes` (`device_id`, `name`, `kind`, `title`, `mml`), up to 24 | That Wignet only |

Each has a `kind`. An **alarm** tune loops for the whole ring, like a written
one (built in: First Light). A **chime** lasts 10 s at most and plays once
(built in: Sparkle, Whoosh, Ta-da, Good news, All done, Doorbell). The
firmware has no play-once mode: it loops a tune for `max_duration_seconds`,
30 s at least, after a fade-in from silence. So a chime is stored followed by
whole-bar rests on every channel, making one loop last 30 s or a little more,
and its event gets `fade_in_seconds: 0` and `max_duration_seconds: 30`
whatever the caller sent. It plays once at full volume, and the ring ends in
the silence; the frame counts as ringing for those 30 s (a button ends it).
A firmware flag would remove the silent tail; this needs none.

Built-in tunes ship with the repository and reach every owner: only original
or public-domain music goes there. Saved tunes are the owner's own data,
kept per Wignet because the owner surfaces (MCP, REST) are addressed by the
Wignet and carry no account.

Agents use `get_tunes` (also `GET /api/owner/<id>/tunes`: each tune with
its kind, MML and a `preview_url`), `save_tune` (`name`, `title`, `tune`,
`kind`; the same name replaces it) and `delete_tune`. They are listed only for a frame whose
capabilities say `tune`. In `update_event`, a new `tune_name` replaces the old
MML and a new `tune` drops the old name. The admin event editor offers the
same list in a selector. Only the agent platform expands names; the
historical runtime refuses a `tune_name` it cannot resolve.

## Structure

- The first line may be `#MML1`. The platform adds it when missing; the
  firmware requires it.
- Every other non-empty line starts with a channel letter and a space:

  | Channel | Sound | `@` timbre |
  | --- | --- | --- |
  | `A`, `B` | Pulse wave | duty 0 = 12.5 %, 1 = 25 %, **2 = 50 %**, 3 = 75 % |
  | `C` | Wave | **0 = triangle**, 1 = sawtooth |
  | `D` | Noise (percussion) | **0 = white**, 1 = metallic (short LFSR) |

  A channel may span several lines; they play one after another. Unused
  channels stay silent.
- `;` starts a comment. `|` (a bar line) and spaces are ignored.
- Commands are case-insensitive; the channel letter must be uppercase.

## Commands

Defaults in bold.

| Command | Meaning |
| --- | --- |
| `c d e f g a b` | Note. Then `+` or `#` (sharp) or `-` (flat), a length, dots. |
| `r` | Rest, with a length and dots like a note. |
| length | `1 2 4 8 16 32 64`, or triplet-style `3 6 12 24 48`. None: the `l` length. Each dot adds half the previous addition; a dotted length must stay a whole number of ticks (`64.` is fine, `64..` is not). |
| `&` | Tie: `c4&c8` is one note lasting a quarter and an eighth. Both sides must be the same pitch. |
| `o1`–`o7` | Octave, **`o4`**; `o4 c` is middle C (MIDI 60). Playable range: `o1 c` to `o7 b`. |
| `>` `<` | One octave up, one down. |
| `l` | Default length, **`l4`**; dots allowed (`l8.`). |
| `t40`–`t240` | Tempo in quarter notes per minute, **`t120`**. Before the channel's first note; every channel that sets it must agree. |
| `v0`–`v15` | Volume, **`v12`**. |
| `@` | Timbre, see the channel table. |
| `s0`–`s7` | Envelope: each note's volume falls one step every n/64 s (Game Boy style). **`s0`** holds it. |
| `q1`–`q8` | Gate: a note sounds for q/8 of its length, the rest is silence, **`q7`**. `q8` is legato. |
| `[ … ]n` | Repeat n times, 2–16, **2**. Nests 4 deep. Opens and closes on the same line. |

## Rules

- **Every channel lasts exactly as long** as the others: the tune loops, and
  a short channel would drift. Pad with rests. The error says by how many
  beats: `channel B lasts 15 beats but channel A lasts 16`.
- At least one note. At most 180 s per loop (it repeats for the whole
  alarm anyway) and **2048 bytes once normalized**: the NVS copy on the frame
  shares a 20 KB partition.
- Loops are bounded: 50,000 commands per channel with loops expanded.

Errors name the input line and quote the text around the problem, for
example `line 3 near "c4 h4": unknown command 'h'`.

## Normalized form

What the platform stores, returns in `get_alarm_schedule`, and sends to the
frame: the `#MML1` header, then one `X commands` line per input line, with
comments removed, whitespace collapsed to single spaces and commands
lowercased. It parses to exactly the same notes as the input.

It never contains `"`, `\`, `{` or `}`. The firmware's JSON reader cuts the
alarm object at the first `}` and reads strings up to the closing quote
(decoding `\n` escapes), so these characters must never reach it.

## Timing and synthesis

96 ticks per quarter note. A tick lands on sample
`tick * 48000 * 60 / (tempo * 96)`, computed from the tune's start, so long
tunes do not drift. Each channel spans 16,000 peak to peak at `v15`; the
four-channel mix is clamped to 16 bits and then scaled by the alarm's fade-in and volume,
like the WAV. Pulse waves are offset so every duty averages to zero, which
avoids clicks at note edges. The noise channel's pitch sets its
shift-register clock (eight times the note frequency, capped below the sample
rate): higher notes sound brighter. Synthesis runs in the existing I2S loop at
10 ms per buffer, so the fade and the buttons behave as they do for a WAV.

## Delivery and fallback

- `/schedule` includes `alarm.tune` (text or `null`) only when the frame
  reported `tune_mml1`. Older firmware never sees it and keeps its WAV.
- Setting a tune on a frame that has not reported `tune_mml1` is refused
  (`field: "tune"`); clearing one (`null`) is always allowed.
- The frame saves the tune to NVS only when it changes.
- At ring time the frame validates the tune again. If it is refused, the
  frame rings the WAV, and if the WAV is missing, the generated tone.
- A tune saved while the frame sleeps arrives with the next poll, at most 15
  minutes later when an alarm is enabled. Until then the previously cached
  alarm rings with its previous sound.
