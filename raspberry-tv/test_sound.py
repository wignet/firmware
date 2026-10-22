"""Sound on the TV: the held list, the synthesizer and the player.

`mml.py` is the platform's own parser, copied here. The reference
synthesizer is the wignet-tunes skill's `tune.py`: point WIGNET_TUNE_PY at it
to compare samples, else that test is skipped. The synthesizer and player
tests need numpy.
"""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
if os.environ.get("WIGNET_TUNE_PY"):
    sys.path.insert(1, str(Path(os.environ["WIGNET_TUNE_PY"]).parent))

import sound_list  # noqa: E402
from tv_power import TVPower  # noqa: E402
from wignet_tv import EventWakeScheduler  # noqa: E402

try:
    import numpy  # noqa: F401
    import mml
    import sound_player
    import tune_render
except ImportError:
    tune_render = sound_player = mml = None

FIXTURES = sorted((HERE / "fixtures" / "mml").glob("*.mml"))


def entry(occurrence, epoch, pushed=False, **fields):
    raw = {"occurrenceId": occurrence, "eventId": "e", "epoch": epoch, "pushed": pushed,
           "sound": "morning_elasticity", "volume": 50, "fadeInSec": 0, "maxDurationSec": 30}
    raw.update(fields)
    return sound_list.parse_entry(raw)


class SoundListTests(unittest.TestCase):
    def test_due_within_grace_only_and_not_twice(self):
        held = [entry("a", 1000), entry("b", 2000)]
        self.assertIsNone(sound_list.due(held, [], 999))
        self.assertEqual(sound_list.due(held, [], 1000)["occurrenceId"], "a")
        self.assertEqual(sound_list.due(held, [], 1120)["occurrenceId"], "a")
        self.assertIsNone(sound_list.due(held, [], 1121))
        self.assertIsNone(sound_list.due(held, ["a"], 1000))
        self.assertEqual(sound_list.next_epoch(held, [], 1000), 2000)
        self.assertIsNone(sound_list.next_epoch(held, ["b"], 1500))

    def test_answer_replaces_planned_sounds_but_keeps_an_unrung_push(self):
        held = [entry("push", 1000, pushed=True), entry("old", 5000)]
        merged = sound_list.merge(held, [entry("new", 6000)], [], 1050)
        self.assertEqual([item["occurrenceId"] for item in merged], ["push", "new"])
        self.assertEqual(sound_list.merge(held, [], ["push"], 1050), [])
        self.assertEqual(sound_list.merge(held, [], [], 1121), [])

    def test_malformed_entries_are_dropped(self):
        answer = sound_list.parse_answer({"sounds": [{"epoch": 5}, {"occurrenceId": "x", "epoch": "no"},
                                                     {"occurrenceId": "ok", "epoch": 5, "volume": 101},
                                                     {"occurrenceId": "y", "epoch": 7, "tuneId": "zz"}],
                                          "tunes": {"abc": "#MML1", "../x": "bad"}})
        entries, tunes = answer
        self.assertEqual([(item["occurrenceId"], item["tuneId"]) for item in entries], [("y", "")])
        self.assertEqual(tunes, {"abc": "#MML1"})
        self.assertIsNone(sound_list.parse_answer({"nextRefreshSec": 60}))

    def test_rung_occurrences_survive_a_restart(self):
        with tempfile.TemporaryDirectory() as state:
            store = sound_list.Store(state)
            self.assertTrue(store.update([entry("a", 1000, tuneId="abc")], {"abc": "#MML1", "dead": "x"}, 900))
            self.assertFalse(store.update([entry("a", 1000, tuneId="abc")], {"abc": "#MML1"}, 900))
            store.mark_done("a")
            again = sound_list.Store(state)
            self.assertEqual(again.done, ["a"])
            self.assertEqual(again.tunes, {"abc": "#MML1"})
            self.assertIsNone(sound_list.due(again.entries, again.done, 1000))

    def test_rung_set_keeps_the_newest(self):
        done = []
        for number in range(40):
            done = sound_list.mark_done(done, str(number))
        self.assertEqual(done, [str(number) for number in range(8, 40)])


@unittest.skipIf(tune_render is None, "numpy unavailable")
class TuneRenderTests(unittest.TestCase):
    def test_same_samples_as_the_reference_synthesizer(self):
        try:
            import tune
        except ImportError:
            self.skipTest("set WIGNET_TUNE_PY to the wignet-tunes skill's tune.py")
        checked = 0
        for path in FIXTURES:
            with self.subTest(path=path.name):
                parsed = mml.parse(path.read_text(encoding="utf-8"))
                self.assertEqual(tune_render.render(parsed).tolist(), tune.render(parsed, rate=48000))
                checked += 1
        self.assertGreater(checked, 0)

    def test_refused_tune_raises_the_platform_error(self):
        with self.assertRaises(mml.MmlError):
            tune_render.render_text("#MML1\nA t120 c4\nB c2")

    def test_fallback_tone_is_880_hz_beeps(self):
        tone = tune_render.fallback_tone(0.5)
        self.assertEqual(len(tone), 24000)
        self.assertEqual(int(tone[:12000].max()), 12000)
        self.assertFalse(tone[12000:].any())


class FakeCEC:
    def __init__(self, succeeds=True):
        self.succeeds, self.wakes, self.standbys = succeeds, 0, 0
        self.state = "standby"

    def power_status(self):
        return self.state

    def wake_tv(self):
        self.wakes += 1
        if self.succeeds and self.state is not None:
            self.state = "on"
        return self.succeeds

    def standby_tv(self):
        self.standbys += 1
        self.state = "standby"


class FakeTimer:
    def __init__(self, delay, callback, args=()):
        self.delay, self.callback, self.args = delay, callback, args
        self.cancelled = False

    def start(self):
        pass

    def cancel(self):
        self.cancelled = True

    def fire(self):
        if not self.cancelled:
            self.callback(*self.args)


class FakeStdin:
    def __init__(self):
        self.data = bytearray()

    def write(self, data):
        self.data += data

    def close(self):
        pass


class FakeAplay:
    started = []

    def __init__(self, command, stdin=None):
        self.command, self.stdin, self.returncode = command, FakeStdin(), None
        FakeAplay.started.append(self)

    def wait(self):
        self.returncode = 0

    def kill(self):
        pass


@unittest.skipIf(sound_player is None, "numpy unavailable")
class PlayerTests(unittest.TestCase):
    def setUp(self):
        FakeAplay.started = []
        self.state = tempfile.TemporaryDirectory()
        self.addCleanup(self.state.cleanup)
        self.cec = FakeCEC()
        self.clock = 0.0
        self.power = TVPower(self.cec, clock=lambda: self.clock, timer_factory=FakeTimer)
        self.player = sound_player.Player(self.state.name, self.power, device="hdmi", popen=FakeAplay)
        patcher = patch.object(sound_player.time, "monotonic", lambda: self.clock)
        patcher.start()
        self.addCleanup(patcher.stop)

    def answer(self, server_epoch, *entries, tunes=None):
        with patch.object(sound_player.threading, "Thread"):  # render on demand instead
            self.player.update({"nextRefreshSec": 60, "serverEpoch": server_epoch,
                                "sounds": list(entries), "tunes": tunes or {}})

    def display_event(self, remaining=300):
        scheduler = EventWakeScheduler(self.power, FakeTimer)
        scheduler.update({"occurrence": "display", "delay_seconds": 0,
                          "window_remaining_seconds": remaining})
        scheduler.timer.fire()

    def finish(self, expected_end):
        self.assertEqual(self.power.until, expected_end)
        self.clock = expected_end + self.power.grace
        self.power.timer.fire()
        self.assertEqual(self.cec.standbys, 1)

    def test_image_only_event_returns_a_sleeping_tv_to_standby(self):
        self.display_event()
        self.assertEqual(self.cec.wakes, 1)
        self.assertEqual(FakeAplay.started, [])
        self.finish(300)

    def test_image_then_sound_waits_for_the_existing_wake_without_restarting_it(self):
        self.display_event()
        self.answer(1000, {"occurrenceId": "a", "epoch": 1000,
                           "fadeInSec": 0, "maxDurationSec": 30})
        self.assertEqual(self.player.step(), 8)
        self.clock = 4
        self.assertEqual(self.player.step(), 4)
        self.assertEqual(FakeAplay.started, [])
        self.clock = 8
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(len(FakeAplay.started), 1)
        self.assertEqual(self.cec.wakes, 1)
        self.finish(300)

    def test_sound_then_image_keeps_the_same_readiness_and_longer_display_hold(self):
        self.answer(1000, {"occurrenceId": "a", "epoch": 1000,
                           "fadeInSec": 0, "maxDurationSec": 30})
        self.assertEqual(self.player.step(), 8)
        self.clock = 3
        self.display_event(remaining=297)
        self.assertEqual(self.player.step(), 5)
        self.assertEqual(FakeAplay.started, [])
        self.clock = 8
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(len(FakeAplay.started), 1)
        self.assertEqual(self.cec.wakes, 1)
        self.finish(300)

    def test_long_sound_keeps_the_tv_on_after_the_display_window_ends(self):
        self.display_event(remaining=120)
        self.answer(1000, {"occurrenceId": "a", "epoch": 1000,
                           "fadeInSec": 0, "maxDurationSec": 300})
        self.assertEqual(self.player.step(), 8)
        self.clock = 8
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(self.cec.wakes, 1)
        self.finish(308)

    def test_image_and_sound_leave_a_tv_already_in_use_on(self):
        self.cec.state = "on"
        self.display_event()
        self.answer(1000, {"occurrenceId": "a", "epoch": 1000,
                           "fadeInSec": 0, "maxDurationSec": 30})
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(len(FakeAplay.started), 1)
        self.assertEqual((self.cec.wakes, self.cec.standbys), (0, 0))
        self.assertIsNone(self.power.timer)

    def test_unreadable_tv_shares_its_startup_delay_without_claiming_standby(self):
        self.cec.state = None
        self.display_event()
        self.answer(1000, {"occurrenceId": "a", "epoch": 1000,
                           "fadeInSec": 0, "maxDurationSec": 30})
        self.assertEqual(self.player.step(), 8)
        self.assertEqual(FakeAplay.started, [])
        self.clock = 8
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(len(FakeAplay.started), 1)
        self.assertEqual(self.cec.wakes, 1)
        self.assertIsNone(self.power.timer)

    def test_wakes_tv_ahead_then_rings_once_at_its_time(self):
        self.answer(1000, {"occurrenceId": "a", "epoch": 1030, "volume": 100,
                           "fadeInSec": 0, "maxDurationSec": 2, "sound": "none"})
        self.assertEqual(self.player.step(), 22)
        self.assertEqual(self.cec.wakes, 0)
        self.clock = 22.0
        self.player.step()
        self.assertEqual(self.cec.wakes, 1)
        self.clock = 30.0
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(len(FakeAplay.started), 1)
        aplay = FakeAplay.started[0]
        self.assertEqual(aplay.command[-2:], ["-D", "hdmi"])
        self.assertEqual(len(aplay.stdin.data), 2 * 48000 * 2)
        self.assertEqual(self.player.step(), sound_player.IDLE_SECONDS)
        self.assertEqual(self.cec.wakes, 1)
        self.assertEqual(sound_list.Store(self.state.name).done, ["a"])
        self.finish(142)  # woken at 22; the 2-minute minimum exceeds this short sound

    def test_pushed_sound_waits_for_the_tv_it_just_woke(self):
        self.answer(1000, {"occurrenceId": "p", "epoch": 1000, "pushed": True,
                           "fadeInSec": 0, "maxDurationSec": 1})
        self.assertEqual(self.player.step(), 8)
        self.assertEqual(FakeAplay.started, [])
        self.clock = 8.0
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(len(FakeAplay.started), 1)

    def test_without_cec_rings_at_once(self):
        self.cec.succeeds = False
        self.answer(1000, {"occurrenceId": "p", "epoch": 1000, "pushed": True,
                           "fadeInSec": 0, "maxDurationSec": 1})
        self.assertEqual(self.player.step(), 0)
        self.assertEqual(len(FakeAplay.started), 1)

    def test_tune_loops_with_fade_in_and_is_cached(self):
        tune = "#MML1\nA t120 o4 l4 c e g >c"
        self.cec.succeeds = False
        self.answer(1000, {"occurrenceId": "t", "epoch": 1000, "volume": 50, "fadeInSec": 1,
                           "maxDurationSec": 3, "tuneId": "abc123"}, tunes={"abc123": tune})
        self.player.step()
        played = numpy.frombuffer(FakeAplay.started[0].stdin.data, dtype="<i2")
        loop = tune_render.render_text(tune)
        self.assertEqual(len(played), 4 * 48000)
        # 0.1 s into a 1 s fade at volume 50: at most 0.5 % of full scale.
        self.assertLess(int(numpy.abs(played[:4800]).max()), int(numpy.abs(loop).max()) // 100)
        late = 2 * 48000 + 100
        self.assertAlmostEqual(int(played[late]), round(loop[late % len(loop)] * 0.5), delta=1)
        self.assertTrue((Path(self.state.name) / "tunes" / "abc123.pcm").is_file())

    def test_gain_follows_the_square_of_the_fade(self):
        gains = sound_player.gain(numpy.array([0, 50, 100, 200]), 80, 100)
        self.assertEqual(gains.round(3).tolist(), [0.0, 0.2, 0.8, 0.8])


if __name__ == "__main__":
    unittest.main()
