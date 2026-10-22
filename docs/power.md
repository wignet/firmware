# Frame power: plugged in or on battery

A frame's power source, not its alarms, decides how often it syncs with the
server. A **plugged-in** frame syncs every `powered_sync_interval_seconds`:
10 s by default (60 s before 2026-10-05), up to 3600 s if the owner sets it. A frame **on battery**
syncs every `battery_sync_interval_seconds`, set by the owner. Syncing never means
redrawing: the screen changes only when the image did (its ETag).

Code: `backoffice/power.py`. Plan and rollout: GitHub issue #66, step 3.
Server side since 2026-09-29; the PhotoPainter firmware reports since 2026-09-30.

## Where the answer comes from

`power.state(device, params)` takes the first of these:

| Source | Where | Notes |
| --- | --- | --- |
| `reported` | `devices.reported_power` and the next columns, from `/schedule` `pwr=` | Firmware that can sense it. Today only the PhotoPainter (AXP2101 power chip), since 2026-09-30 |
| `stated` | `params["power_source"]` (`powered` / `battery`) | The owner says so, with `set_power_source`. Only offered for frames that don't report, and ignored once one does |
| `default` | none | `battery`: how every frame behaved before, so no battery frame is drained by surprise |

A frame declaring `stays_awake1` (the LCD boards, `WIGNET_ALWAYS_POWERED`)
never sleeps, so `power.is_powered` treats it as powered whatever it reports:
it keeps the powered cadence and slide behaviour on battery. `pwr=` then
only says what the owner sees (`get_state` power: `state`, and
`stays_awake: true`), until battery power management lands.

Which boards can sense power (checked in the firmware, 2026-09-29):

| Board | Plugged in? | Battery level? |
| --- | --- | --- |
| PhotoPainter (ESP32-S3 + AXP2101) | Yes | Yes (voltage, fuel gauge not yet calibrated) |
| ESP32-S3-LCD-1.54 (divider on GPIO1, charge pin GPIO3) | Partly, since 2026-10-19: `pwr=ext` while charging or while a computer is on its USB, else `pwr=bat` (a wall charger with a full cell reads as battery) | Yes, since 2026-10-17: voltage, a percentage from a LiPo curve, charging |
| FireBeetle 7.5" (DFR0478) | Probably not | Maybe, through a voltage divider: to check on the board |
| Waveshare driver, D1 mini | No | No |

## What `/schedule` does with it

`build_schedule_response` records the report (`power.record`), then:

- **powered:** `nextRefreshSec` is the powered cadence
  (`power.powered_sync_seconds`, 10 s unless the owner set up to 3600 s). The
  agent platform (`content_runtime.cadence`) answers it in place of the battery
  interval, then shortens it at event edges; the legacy `/schedule` caps the
  service interval with it. Shorter answers (a render retry, a pushed sound)
  stand.
- **Below 60 s, what the frame does** depends on its firmware. The PhotoPainter
  from 2026-09-30 stays awake on mains and polls at `nextRefreshSec`, 10 s at
  least. Older PhotoPainter builds poll every 60 s on mains whatever the answer.
  A frame that doesn't report power (stated as plugged in) deep-sleeps for an
  answer above 30 s, plus about 20 s of stability delay and reconnection, and
  stays awake polling every 60 s for 30 s or less: in practice about a minute
  at best. The setting is still stored, for the day the board runs firmware
  that reports power, but `power.syncs_every_seconds` shows at least 60 s for
  such a frame (`power.effective_powered_seconds`) and
  `set_powered_sync_interval` warns below 60 s, so an agent does not promise
  a delivery in 10 s. Old PhotoPainter builds do report power and are not
  caught by this: reflash them.
- **The legacy « veille active »** (`active_watch_enabled` +
  `active_watch_interval_sec`, in `schedule.py`, removed in #101) only ever changed
  `nextRefreshSec`, so it never got past the firmware limits above either.
  The agent platform does not read it: `powered_sync_interval_seconds`
  replaces it for a plugged-in frame, and a battery frame had no business
  polling that often.
- **battery:** unchanged. The service interval or `battery_sync_interval_seconds`,
  shortened by timing rules. Firmware without `sound_list1` is still capped at
  15 minutes while an alarm is due (`ALARM_SYNC_MAX_SECONDS`); a frame
  holding the list of sounds is not (step 5 of #66, 2026-10-01).

A poll without `pwr` after one with it clears the report, as a firmware
downgrade clears reported capabilities. An invalid `pwr` keeps the previous
report. `hw=` (the build target) is stored in `devices.reported_hw`.

**History:** `device_power_log` keeps a row when the source, level or
charging state changes, and at most one an hour otherwise (millivolts move on
every sync). `get_power_history` (MCP) and `GET /api/owner/<mac>/power_log?limit=`
(also `/get_power_history`) return it newest first, with `latest_battery_run`: the latest stretch on
battery, its hours, voltage and gauge drop, and `percent_per_day`. With no
power meter on the bench, that is how a frame's drain is measured: leave it
unplugged for some hours and read the run, in `mv` rather than the gauge's
percentage. A first reading, 2026-10-09: on the PhotoPainter, about 5 days at
one sync a minute and about 40 at one an hour (`HARDWARE.md`,
"Battery drain"; issue #182). On the agent platform (the default
runtime since 2026-09-30) both are `platform_api.power_history`; the old
runtime's `owner_actions.get_power_history` stays for the rollback period.

## Names

`battery_sync_interval_seconds` is what people and agents see. It is stored
as `params["interval_seconds"]`, the key the admin UI and the legacy
scheduler already read, so nothing else moved.

| Where | Name |
| --- | --- |
| MCP tool, REST route | `set_battery_sync_interval` (`set_interval` still works, and is no longer listed on the MCP); `set_powered_sync_interval` (10–3600 s, stored as `params["powered_sync_interval_seconds"]`; absent means 10 s) |
| State | `power.battery_sync_interval_seconds`, `power.powered_sync_interval_seconds`, and `power.syncs_every_seconds`, what the frame really does (the powered cadence when plugged in, 60 s at best for a frame that doesn't report power; the alarm cap counted) |
| Firmware field | `nextRefreshSec`, unchanged: installed firmware parses it |

## What the owner and agents see

- `get_state` → `power`: `state`, `source`, `battery_percent`, `battery_mv`,
  `charging`, `reported_at`, `hardware`, `can_state`,
  `battery_sync_interval_seconds`, `powered_sync_interval_seconds`,
  `syncs_every_seconds`.
- `set_powered_sync_interval` (`seconds`, 10 to 3600) and the admin page's
  « Synchronisation sur secteur » field. Shorter means content shows sooner
  on a plugged-in frame, for more requests; the screen still redraws only
  when the image changed. Refused outside 10–3600; a stored value outside
  that range counts as 10 s.
- `set_power_source` (`powered`, `battery`, or `null` to forget it). Not
  listed on the MCP, nor in the OpenAPI spec or the guide, for a frame that
  reports power, and refused there if called.
- Warnings in the reply, never refusals: on battery, an interval above 900 s
  while an alarm is due, from `set_battery_sync_interval`, `set_events` and
  `update_event`. On a plugged-in frame, `set_battery_sync_interval` says the
  setting only applies on battery.
- The skills read `power.syncs_every_seconds` to say when something will
  appear; `wignet-alarm` advises staying under 15 minutes on battery only for
  firmware without `sound_list` (`get_capabilities`).

Not done yet: the owner page's power line and one-time question.
