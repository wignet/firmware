import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError


sys.path.insert(0, str(Path(__file__).parent))
SPEC = importlib.util.spec_from_file_location("wignet_tv", Path(__file__).with_name("wignet_tv.py"))
wignet_tv = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = wignet_tv
SPEC.loader.exec_module(wignet_tv)


class Response:
    def __init__(self, data, content_type="image/png", etag=None, headers=None):
        self.data = data
        self.content_type = content_type
        self.etag = etag
        self.extra_headers = headers or {}

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    @property
    def headers(self):
        return self

    def get_content_type(self):
        return self.content_type

    def get(self, name, default=None):
        if name == "ETag":
            return self.etag
        return self.extra_headers.get(name, default)

    def read(self):
        return self.data


class TvClientTests(unittest.TestCase):
    def setUp(self):
        self.settings = wignet_tv.Settings("https://example.test", "02:00:00:00:00:09", 1920, 1080)

    def test_only_schedule_declares_capabilities(self):
        self.assertNotIn("caps=", self.settings.image_url)
        self.assertIn("caps=v1%3Argb%2Cscreen_size1%2Cpower_report1", self.settings.schedule_url)
        self.assertIn("hw=raspberry_pi_hdmi", self.settings.schedule_url)

    def test_firmware_version_goes_on_schedule_only(self):
        self.assertNotIn("fw=", self.settings.schedule_url)
        versioned = wignet_tv.Settings("https://example.test", "02:00:00:00:00:09", 1920, 1080,
                                       fw="281f8374795a-dirty.140a4019")
        self.assertIn("fw=281f8374795a-dirty.140a4019", versioned.schedule_url)
        self.assertNotIn("fw=", versioned.image_url)

    def test_installed_version_is_read_and_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "wignet_tv.py"
            with patch.object(wignet_tv, "__file__", str(script)):
                self.assertEqual(wignet_tv.installed_version(), "")
                (Path(tmp) / "VERSION").write_text("281f8374795a\n")
                self.assertEqual(wignet_tv.installed_version(), "281f8374795a")
                (Path(tmp) / "VERSION").write_text("not a version")
                self.assertEqual(wignet_tv.installed_version(), "")

    def test_sound_capabilities_are_declared_only_with_a_player(self):
        sound = wignet_tv.Settings("https://example.test", "02:00:00:00:00:09", 1920, 1080,
                                   caps=wignet_tv.SOUND_CAPS)
        self.assertIn("wake_alarm%2Caudio_out%2Csound_list1%2Ctune_mml1", sound.schedule_url)
        self.assertNotIn("caps=", sound.image_url)
        self.assertNotIn("audio_out", self.settings.schedule_url)
        with patch.dict(wignet_tv.os.environ, {"WIGNET_SOUND": "off"}):
            self.assertIsNone(wignet_tv.start_player("/nonexistent", None))

    def test_schedule_answer_reaches_the_player(self):
        answers = []

        class Player:
            def update(self, payload):
                answers.append(payload)

        with patch.object(wignet_tv, "urlopen",
                          return_value=Response(b'{"nextRefreshSec": 60, "sounds": []}',
                                                "application/json")):
            self.assertEqual(wignet_tv.next_seconds(self.settings, Player()), 60)
        self.assertEqual(answers, [{"nextRefreshSec": 60, "sounds": []}])

    def test_schedule_carries_the_ota_fields_and_hands_it_the_answer(self):
        class Ota:
            answers = []

            def query(self):
                return {"on": "ab" * 16, "ota": "pending:0123456789abcdef", "oc": 7}

            def answer(self, payload, seconds):
                self.answers.append((payload, seconds))
                return 30

        ota = Ota()
        with patch.object(wignet_tv, "urlopen",
                          return_value=Response(b'{"nextRefreshSec": 600}', "application/json")) as opened:
            self.assertEqual(wignet_tv.next_seconds(self.settings, ota=ota), 30)
        url = opened.call_args[0][0].full_url
        self.assertIn("on=" + "ab" * 16, url)
        self.assertIn("ota=pending%3A0123456789abcdef&oc=7", url)
        self.assertEqual(ota.answers, [({"nextRefreshSec": 600}, 600)])
        # No answer at all: the updater still sees the poll, for its deadline.
        with patch.object(wignet_tv, "urlopen", side_effect=URLError("down")), \
             patch.object(sys, "stderr"):
            self.assertEqual(wignet_tv.next_seconds(self.settings, ota=ota), 30)
        self.assertEqual(ota.answers[-1], (None, wignet_tv.FALLBACK_SECONDS))

    def test_ota_stays_off_without_the_ab_image(self):
        with patch.dict(wignet_tv.os.environ, {"WIGNET_TV_OTA": ""}):
            self.assertIsNone(wignet_tv.start_ota("02:00:00:00:00:09", "/nonexistent"))

    def test_a_short_answer_near_an_event_edge_is_followed_not_replaced_by_the_fallback(self):
        # The server answers the seconds to the next edge (1 to 9 near it): a
        # powered HDMI client redraws at once, so it polls then rather than
        # waiting out the 60 s fallback with an expired text still shown.
        for answer, expected in ((b'{"nextRefreshSec": 1}', 1), (b'{"nextRefreshSec": 9}', 9),
                                 (b'{"nextRefreshSec": 0}', 60), (b'{"nextRefreshSec": "5"}', 60),
                                 (b'{"nextRefreshSec": 86401}', 60)):
            with self.subTest(answer=answer), \
                 patch.object(wignet_tv, "urlopen",
                              return_value=Response(answer, "application/json")), \
                 patch.object(sys, "stderr"):
                self.assertEqual(wignet_tv.next_seconds(self.settings), expected)

    def test_startup_progress_row_is_rgb565_and_clamps_segment(self):
        row = wignet_tv.progress_row(4, 9, 99)

        self.assertEqual(len(row), 4 * 2)
        self.assertEqual(row, wignet_tv._rgb565(wignet_tv.CORAL) * 4)

    def test_startup_progress_row_keeps_track_on_both_sides(self):
        row = wignet_tv.progress_row(5, 2, 2)
        track = wignet_tv._rgb565(wignet_tv.BORDER)
        accent = wignet_tv._rgb565(wignet_tv.CORAL)

        self.assertEqual(row, track * 2 + accent * 2 + track)

    def test_image_etag_is_sent_and_304_keeps_frame(self):
        error = HTTPError(self.settings.image_url, 304, "Not Modified", {}, None)
        with patch.object(wignet_tv, "urlopen", side_effect=error) as request:
            data, etag, unchanged, wake_event = wignet_tv.get_image(self.settings, '"known"')
        self.assertIsNone(data)
        self.assertEqual(etag, '"known"')
        self.assertTrue(unchanged)
        self.assertIsNone(wake_event)
        self.assertEqual(request.call_args.args[0].headers["If-none-match"], '"known"')

    def test_event_response_headers_identify_an_occurrence_and_relative_wake_delay(self):
        headers = {"X-Wignet-Event-Occurrence": "0123456789abcdef01234567",
                   "X-Wignet-Event-Wake-After": "284"}
        response = Response(b"PNG", etag='"event"', headers=headers)
        with patch.object(wignet_tv, "urlopen", return_value=response):
            data, etag, unchanged, wake_event = wignet_tv.get_image(self.settings)
        self.assertEqual((data, etag, unchanged), (b"PNG", '"event"', False))
        self.assertEqual(wake_event, {"occurrence": "0123456789abcdef01234567", "delay_seconds": 284})

    def test_event_wake_metadata_survives_an_unchanged_304_response(self):
        headers = {"X-Wignet-Event-Occurrence": "0123456789abcdef01234567",
                   "X-Wignet-Event-Wake-After": "120"}
        error = HTTPError(self.settings.image_url, 304, "Not Modified", headers, None)
        with patch.object(wignet_tv, "urlopen", side_effect=error):
            data, etag, unchanged, wake_event = wignet_tv.get_image(self.settings, '"event"')
        self.assertIsNone(data)
        self.assertTrue(unchanged)
        self.assertEqual(wake_event, {"occurrence": "0123456789abcdef01234567", "delay_seconds": 120})

    def test_invalid_or_absent_event_metadata_does_not_request_a_wake(self):
        self.assertIsNone(wignet_tv.event_wake({}))
        self.assertIsNone(wignet_tv.event_wake({"X-Wignet-Event-Occurrence": "bad",
                                                "X-Wignet-Event-Wake-After": "0"}))
        self.assertIsNone(wignet_tv.event_wake({"X-Wignet-Event-Occurrence": "0123456789abcdef01234567",
                                                "X-Wignet-Event-Wake-After": "3601"}))

    def test_event_wake_scheduler_fires_once_and_slide_metadata_cancels_pending_wake(self):
        timers = []

        class Timer:
            def __init__(self, delay, callback, args=()):
                self.delay, self.callback, self.args = delay, callback, args
                self.cancelled = False
                timers.append(self)

            def start(self):
                pass

            def cancel(self):
                self.cancelled = True

            def fire(self):
                if not self.cancelled:
                    self.callback(*self.args)

        class CEC:
            def __init__(self):
                self.wakes = 0

            def wake_tv(self):
                self.wakes += 1

        cec = CEC()
        scheduler = wignet_tv.EventWakeScheduler(cec, Timer)
        event = {"occurrence": "event-one", "delay_seconds": 300}
        scheduler.update(event)
        scheduler.update(event)  # repeated polls do not postpone the same wake
        self.assertEqual(len(timers), 1)
        self.assertEqual(timers[0].delay, 300)
        timers[0].fire()
        scheduler.update(event)
        self.assertEqual(cec.wakes, 1)
        scheduler.update({"occurrence": "event-two", "delay_seconds": 60})
        scheduler.update(None)  # no event (for example, the slideshow): cancel it
        timers[1].fire()
        self.assertEqual(cec.wakes, 1)

    def test_displayed_etag_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            wignet_tv.save_etag(directory, '"frame"')
            self.assertEqual(wignet_tv.load_etag(directory), '"frame"')

    def test_splash_cache_is_the_frame_boot_splash_reads(self):
        class Display:
            width, height, stride = 4, 2, 8
        script = Path(__file__).with_name("boot-splash.sh").read_text()
        self.assertIn("splash-${size%,*}x${size#*,}-$stride.rgb565", script)
        with tempfile.TemporaryDirectory() as directory:
            stale = Path(directory) / "splash-9x9-18.rgb565"
            stale.write_bytes(b"old")
            wignet_tv.save_splash(directory, Display(), b"\1" * 16)
            cached = Path(directory) / "splash-4x2-8.rgb565"
            self.assertEqual(cached.read_bytes(), b"\1" * 16)
            self.assertFalse(stale.exists())

    def test_poll_displays_new_server_frame_before_asking_schedule(self):
        order = []

        class Display:
            def page(self, data):
                order.append(("display", data))

        with tempfile.TemporaryDirectory() as directory, \
             patch.object(wignet_tv, "get_image", side_effect=lambda *_: (b"PNG", '"fresh"', False, None)), \
             patch.object(wignet_tv, "next_seconds", side_effect=lambda *_: order.append(("schedule", None)) or 42):
            etag, delay = wignet_tv.poll_once(self.settings, Display(), '"old"', directory)

        self.assertEqual(etag, '"fresh"')
        self.assertEqual(delay, 42)
        self.assertEqual(order, [("display", b"PNG"), ("schedule", None)])

    def test_startup_fetch_ignores_persisted_etag_until_a_frame_is_drawn(self):
        requested = []

        class Display:
            def page(self, data):
                pass

        with tempfile.TemporaryDirectory() as directory, \
             patch.object(wignet_tv, "get_image",
                          side_effect=lambda _settings, etag: requested.append(etag) or (b"PNG", '"fresh"', False, None)), \
             patch.object(wignet_tv, "next_seconds", return_value=42):
            wignet_tv.poll_once(self.settings, Display(), '"persisted"', directory,
                                first_frame=True)

        self.assertEqual(requested, [None])


class SupportLogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "support.log"
        self.stream = io.StringIO()

    def test_whole_lines_are_kept_with_their_time_and_still_reach_syslog(self):
        log = wignet_tv.SupportLog(self.stream, self.path, clock=lambda: 1792184401)
        print("sound: ringing x", file=log)
        log.write("CEC Image View On")   # a line in two writes is kept once whole
        self.assertEqual(self.path.read_text(), "2026-10-16T21:00:01Z sound: ringing x\n")
        log.write(" and Active Source sent\n")
        self.assertEqual(self.path.read_text().splitlines()[-1],
                         "2026-10-16T21:00:01Z CEC Image View On and Active Source sent")
        self.assertEqual(self.stream.getvalue(),
                         "sound: ringing x\nCEC Image View On and Active Source sent\n")

    def test_the_file_is_bounded_with_one_previous_copy(self):
        log = wignet_tv.SupportLog(self.stream, self.path, max_bytes=100, clock=lambda: 0)
        for n in range(20):
            print(f"line {n:02d} " + "x" * 20, file=log)
        previous = self.path.with_name("support.log.1")
        self.assertLessEqual(self.path.stat().st_size, 100 + 40)
        self.assertLessEqual(previous.stat().st_size, 100 + 40)
        self.assertIn("line 19", self.path.read_text())
        self.assertEqual(sorted(p.name for p in self.path.parent.iterdir()),
                         ["support.log", "support.log.1"])

    def test_an_unwritable_file_never_stops_the_client(self):
        log = wignet_tv.SupportLog(self.stream, Path(self.tmp.name) / "missing" / "support.log")
        print("image unavailable", file=log)
        self.assertEqual(self.stream.getvalue(), "image unavailable\n")


if __name__ == "__main__":
    unittest.main()
