import unittest
from unittest.mock import patch

import cec
from tv_power import TVPower


class FakeCEC:
    def __init__(self, state="standby"):
        self.state, self.wakes, self.standbys, self.wake_works = state, 0, 0, True

    def power_status(self):
        return self.state

    def wake_tv(self):
        self.wakes += 1
        if self.wake_works:
            self.state = "on"
        return self.wake_works

    def standby_tv(self):
        self.standbys += 1
        self.state = "standby"
        return True


class Clock:
    now = 1000.0

    def __call__(self):
        return self.now


class Timer:
    """Never runs by itself: the test calls `fire` when the clock is due."""
    created = []

    def __init__(self, delay, function):
        self.delay, self.function, self.cancelled, self.daemon = delay, function, False, False
        Timer.created.append(self)

    def start(self):
        pass

    def cancel(self):
        self.cancelled = True


def live(timers):
    return [t for t in timers if not t.cancelled]


class TVPowerTests(unittest.TestCase):
    def setUp(self):
        Timer.created = []
        self.cec = FakeCEC()
        self.clock = Clock()
        self.clock.now = 1000.0
        self.power = TVPower(self.cec, grace=90, min_on=120, max_wakes=3, window=3600,
                             clock=self.clock, timer_factory=Timer)

    def test_a_wake_says_what_it_found_and_sent(self):
        # Support reads these in syslog: the state seen, then the wake itself.
        with self.assertLogs("wignet_tv", level="INFO") as logs:
            self.power.wake_tv(hold_seconds=900)
            self.power.wake_tv(hold_seconds=300)
        self.assertEqual([r.getMessage() for r in logs.records], [
            "TV in standby; waking it for 900 s",
            "TV already woken by Wignet; keeping it on 300 s more",
        ])

    def due(self):
        timer = live(Timer.created)[-1]
        self.clock.now += timer.delay
        timer.function()

    def test_wakes_a_sleeping_tv_and_puts_it_back_after_the_hold_and_grace(self):
        self.assertTrue(self.power.wake_tv(hold_seconds=300))
        self.assertEqual(self.cec.wakes, 1)
        self.assertEqual(live(Timer.created)[-1].delay, 390)
        self.due()
        self.assertEqual(self.cec.standbys, 1)

    def test_readiness_is_shared_without_restarting_the_delay(self):
        self.assertTrue(self.power.wake_tv())
        self.assertEqual(self.power.ready_in(), 8)
        self.clock.now += 3
        self.assertFalse(self.power.wake_tv(hold_seconds=400))
        self.assertEqual(self.power.ready_in(), 5)
        self.assertEqual(self.cec.wakes, 1)
        self.clock.now += 5
        self.assertEqual(self.power.ready_in(), 0)

    def test_readiness_starts_after_the_cec_commands_finish(self):
        wake = self.cec.wake_tv

        def slow_wake():
            self.clock.now += 4
            return wake()

        with patch.object(self.cec, "wake_tv", side_effect=slow_wake):
            self.assertTrue(self.power.wake_tv())
        self.assertEqual(self.power.ready_in(), 8)
        self.assertEqual(self.power.until, 1304)

    def test_a_tv_someone_switched_on_is_left_alone_now_and_later(self):
        self.cec.state = "on"
        self.assertFalse(self.power.wake_tv())
        self.assertEqual(self.cec.wakes, 0)
        self.assertEqual(live(Timer.created), [])
        self.assertEqual(self.cec.standbys, 0)
        self.assertEqual(self.power.ready_in(), 0)

    def test_an_unreadable_tv_is_woken_but_never_put_to_sleep(self):
        self.cec.state = None
        self.assertTrue(self.power.wake_tv())
        self.assertEqual(self.cec.wakes, 1)
        self.assertEqual(live(Timer.created), [])

    def test_a_second_reason_extends_the_stay_instead_of_cycling(self):
        self.power.wake_tv(hold_seconds=200)
        self.clock.now += 150
        self.assertFalse(self.power.wake_tv(hold_seconds=400))  # already on: no wait
        self.assertEqual(self.cec.wakes, 1)
        self.assertEqual(len(live(Timer.created)), 1)
        self.due()
        self.assertEqual(self.cec.standbys, 1)
        self.assertEqual(self.clock.now, 1000 + 150 + 400 + 90)

    def test_a_short_hold_still_keeps_the_tv_on_for_the_minimum(self):
        self.power.wake_tv(hold_seconds=10)
        self.assertEqual(live(Timer.created)[-1].delay, 120 + 90)

    def test_a_tv_a_person_switched_off_meanwhile_is_not_touched(self):
        self.power.wake_tv(hold_seconds=60)
        self.cec.state = "standby"
        self.due()
        self.assertEqual(self.cec.standbys, 0)

    def test_a_failed_wake_does_not_claim_the_tv(self):
        self.cec.wake_works = False
        self.assertFalse(self.power.wake_tv())
        self.assertEqual(live(Timer.created), [])
        self.assertEqual(self.power.ready_in(), 0)

    def test_loop_guard_stops_waking_after_too_many_cycles_then_recovers(self):
        for _ in range(3):
            self.assertTrue(self.power.wake_tv(hold_seconds=60))
            self.due()
        self.assertEqual((self.cec.wakes, self.cec.standbys), (3, 3))
        self.assertFalse(self.power.wake_tv(hold_seconds=60))
        self.assertEqual(self.cec.wakes, 3)
        self.clock.now += 3600
        self.assertTrue(self.power.wake_tv(hold_seconds=60))
        self.assertEqual(self.cec.wakes, 4)


class PowerStatusTests(unittest.TestCase):
    def status(self, stdout, returncode=0):
        class Result:
            pass

        result = Result()
        result.returncode, result.stdout, result.stderr = returncode, stdout, ""
        with patch.object(cec.Path, "exists", return_value=True), \
             patch.object(cec.Path, "is_file", return_value=True):
            return cec.CEC(True, "/dev/cec0", lambda *a, **k: result).power_status()

    def test_reads_the_reported_state(self):
        self.assertEqual(self.status("Received from TV (0): REPORT_POWER_STATUS\n\tpwr-state: on (0x00)"), "on")
        self.assertEqual(self.status("\tpwr-state: standby (0x01)"), "standby")
        self.assertEqual(self.status("\tpwr-state: to-on (0x02)"), "on")
        self.assertEqual(self.status("\tpwr-state: to-standby (0x03)"), "standby")

    def test_unknown_when_unreadable_or_disabled(self):
        self.assertIsNone(self.status("nothing useful"))
        self.assertIsNone(self.status("", returncode=1))
        self.assertIsNone(cec.CEC(False).power_status())


if __name__ == "__main__":
    unittest.main()
