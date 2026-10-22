"""Wake the TV for an event or a sound, and put it back to sleep afterwards.

The TV is a person's: a television in a bedroom must not be woken for a
passing text, and must not be switched off under someone watching it.  So the
Pi only ever puts to sleep a TV *it* woke.

- It reads the TV's power state before waking.  Already on, someone else's
  business: nothing is sent, now or later.  Unreadable, it wakes as before but
  never switches off, since it cannot tell whose the TV is.
- A TV it woke goes to standby once the last thing it was woken for is over
  (the event's display or the sound), plus a grace period.  A second reason
  arriving meanwhile extends the stay rather than cycling the TV.
- If the TV is already off by then (a person switched it off), it stays so.
- Loop guard: it wakes the TV at most `max_wakes` times per `window` seconds.
  Beyond that it stops waking, and says so, until the window clears.  A faulty
  schedule, or a TV that answers badly, cannot make it flicker all night.
"""
from __future__ import annotations

import logging
import threading
import time

LOG = logging.getLogger("wignet_tv.power")

DEFAULT_HOLD = 300      # seconds an event's display is assumed to need
GRACE = 90              # seconds after the last reason before standby
MIN_ON = 120            # a woken TV stays on at least this long
MAX_WAKES = 4           # wake-ups the Pi will cause ...
WINDOW = 3600           # ... in this many seconds
WAKE_LEAD = 8           # seconds for the TV and its speakers to become ready


class TVPower:
    def __init__(self, cec, grace=GRACE, min_on=MIN_ON, max_wakes=MAX_WAKES, window=WINDOW,
                 clock=time.monotonic, timer_factory=threading.Timer, wake_lead=WAKE_LEAD):
        self.cec = cec
        self.grace = grace
        self.min_on = min_on
        self.max_wakes = max_wakes
        self.window = window
        self.clock = clock
        self.timer_factory = timer_factory
        self.wake_lead = wake_lead
        self.lock = threading.Lock()
        self.owned = False      # this Pi woke the TV and may put it to sleep
        self.until = 0.0        # clock time the last reason ends
        self.wakes = []         # clock times of the wake-ups it caused
        self.timer = None
        self.ready_at = 0.0     # shared by every content that needs this wake

    def wake_tv(self, hold_seconds=DEFAULT_HOLD):
        """Request time on screen; True only when a CEC wake was sent.

        Readiness is separate: a caller must consult ready_in() even if
        another content already woke the TV and this returns False.
        """
        with self.lock:
            now = self.clock()
            if self.owned:
                LOG.info("TV already woken by Wignet; keeping it on %d s more", hold_seconds)
                self._extend_locked(now + hold_seconds)
                return False
            if now < self.ready_at:
                # An unreadable TV may still be starting from our last wake.
                # Wait for that attempt without sending another one; its
                # original state remains unknown, so we still cannot own it.
                return False
            state = self.cec.power_status()
            if state == "on":
                LOG.info("TV already on; leaving it alone")
                return False
            self.wakes = [t for t in self.wakes if now - t < self.window]
            if len(self.wakes) >= self.max_wakes:
                LOG.warning("TV wake refused: %d wake-ups in the last %d s", len(self.wakes), self.window)
                return False
            LOG.info("TV %s; waking it for %d s",
                     "in standby" if state == "standby" else "power unreadable", hold_seconds)
            if not self.cec.wake_tv():
                return False
            # CEC queries and commands can take seconds; start the readiness
            # and hold periods when they finish, not before they were sent.
            now = self.clock()
            self.ready_at = now + self.wake_lead
            self.wakes.append(now)
            # Only a TV seen asleep is ours to put back; an unreadable one is not.
            self.owned = state == "standby"
            if self.owned:
                self.until = max(now + hold_seconds, now + self.min_on)
                self._schedule_locked(now)
            return True

    def ready_in(self):
        """Seconds until a TV we just woke is ready, regardless of the caller."""
        with self.lock:
            return max(0.0, self.ready_at - self.clock())

    def _extend_locked(self, until):
        self.until = max(self.until, until)
        self._schedule_locked(self.clock())

    def _schedule_locked(self, now):
        if self.timer is not None:
            self.timer.cancel()
        timer = self.timer_factory(max(0.0, self.until + self.grace - now), self._due)
        timer.daemon = True
        self.timer = timer
        timer.start()

    def _due(self):
        with self.lock:
            if not self.owned:
                return
            now = self.clock()
            if now < self.until + self.grace:
                self._schedule_locked(now)   # extended while this timer ran
                return
            self.owned = False
            self.timer = None
            if self.cec.power_status() != "on":
                LOG.info("TV already off; nothing to do")
                return
            self.cec.standby_tv()
