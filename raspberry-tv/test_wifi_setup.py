import importlib.util
from pathlib import Path
import sys
import subprocess
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location("wifi_setup", Path(__file__).with_name("wifi_setup.py"))
wifi_setup = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = wifi_setup
SPEC.loader.exec_module(wifi_setup)


class AccessPointTests(unittest.TestCase):
    def test_host_connection_accepts_localized_state(self):
        result = subprocess.CompletedProcess([], 0,
            "GENERAL.STATE:100 (connecté)\nGENERAL.CONNECTION:Lieu\n", "")
        with patch.object(wifi_setup, "nmcli", return_value=result):
            self.assertTrue(wifi_setup.connected())

    def test_setup_ap_is_not_a_host_connection(self):
        result = subprocess.CompletedProcess([], 0,
            f"GENERAL.STATE:100 (connected)\nGENERAL.CONNECTION:{wifi_setup.AP_CONNECTION}\n", "")
        with patch.object(wifi_setup, "nmcli", return_value=result):
            self.assertFalse(wifi_setup.connected())

    def test_existing_wep_setup_profile_is_reset_before_activation(self):
        calls = []
        def command(*args, **_kwargs):
            calls.append(args)
            value = "none\n" if args[:2] == ("-g", "802-11-wireless-security.key-mgmt") else ""
            return subprocess.CompletedProcess(args, 0, value, "")
        with patch.object(wifi_setup, "nmcli", side_effect=command):
            wifi_setup.start_hotspot("Wignet-1234")
        reset = ("connection", "modify", wifi_setup.AP_CONNECTION,
                 "remove", "802-11-wireless-security")
        self.assertIn(reset, calls)
        activation = next(call for call in calls if call[:3] == ("--wait", "25", "connection"))
        self.assertLess(calls.index(reset), calls.index(activation))
        self.assertFalse(any("802-11-wireless-security.key-mgmt" in call
                             for call in calls if call[:2] == ("connection", "modify")))

    def test_failed_ap_activation_closes_portal_without_starting_reader(self):
        with patch.object(wifi_setup, "connected", return_value=False), \
             patch.object(wifi_setup, "wait_for_saved_wifi", return_value=False), \
             patch.object(wifi_setup, "provisioning_ap_name", return_value="Wignet-1234"), \
             patch.object(wifi_setup, "SetupScreen", side_effect=RuntimeError("no framebuffer")), \
             patch.object(wifi_setup, "PortalServer") as server, \
             patch.object(wifi_setup, "start_hotspot", side_effect=RuntimeError("activation failed")), \
             patch.object(wifi_setup, "prepare_connected_runtime") as prepare, \
             self.assertLogs(wifi_setup.LOG, level="WARNING"):
            self.assertEqual(wifi_setup.run_setup(), 1)
        server.return_value.server_close.assert_called_once()
        prepare.assert_not_called()

    def test_bootstrap_retries_failure_until_wifi_setup_completes(self):
        with patch.dict(wifi_setup.os.environ, {}, clear=True), \
             patch.object(wifi_setup, "run_setup", side_effect=[1, 0]) as setup, \
             patch.object(wifi_setup.time, "sleep") as sleep, \
             self.assertLogs(wifi_setup.LOG, level="WARNING"):
            self.assertEqual(wifi_setup.main(), 0)
        self.assertEqual(setup.call_count, 2)
        sleep.assert_called_once_with(5)

    def test_parse_keeps_strongest_network_and_ignores_hidden(self):
        points = wifi_setup.parse_access_points("home:WPA2:40\nhome:WPA2:80\nguest::60\n:WPA2:100\n")
        self.assertEqual(points, [
            wifi_setup.AccessPoint("home", "WPA2", 80),
            wifi_setup.AccessPoint("guest", "", 60),
        ])

    def test_connected_bootstrap_skips_the_setup_portal(self):
        with patch.object(wifi_setup, "connected", return_value=True), \
             patch.object(wifi_setup, "runtime_ready", return_value=False), \
             patch.object(wifi_setup, "install_runtime", return_value=(True, "")):
            self.assertEqual(wifi_setup.main(), 0)


class SetupScreenTests(unittest.TestCase):
    def screen(self, width, height):
        screen = object.__new__(wifi_setup.SetupScreen)
        screen.width, screen.height, screen.stride, screen.previous = width, height, width * 2, None
        return screen

    def test_every_state_says_it_in_english_and_names_the_network(self):
        ready = wifi_setup.screen_lines("Wignet-6909", "ready")
        self.assertIn(("network", "", "Wignet-6909"), ready)
        self.assertEqual(ready[-1], ("footer", "", wifi_setup.PORTAL_HINT))
        self.assertIn(("network", "", "Home"), wifi_setup.screen_lines("Wignet-6909", "connecting", "Home"))
        for state in ("ready", "connecting", "error", "preparing", "failed"):
            for role, _step, text in wifi_setup.screen_lines("Wignet-6909", state, "Home"):
                self.assertIn(role, ("label", "title", "step", "network", "body", "footer"))
                self.assertTrue(text.isascii(), text)

    def test_bitmap_font_draws_every_character_the_screen_says(self):
        for state in ("ready", "connecting", "error", "preparing", "failed"):
            for _role, _step, text in wifi_setup.screen_lines("Wignet-6909", state, "Trying again in 5 seconds."):
                self.assertLessEqual(set(text.upper()), set(wifi_setup.FONT), text)

    def test_bitmap_screen_fills_the_frame_on_black(self):
        data = self.screen(640, 360)._bitmap(wifi_setup.screen_lines("Wignet-6909", "ready"))
        self.assertEqual(len(data), 640 * 360 * 2)
        self.assertEqual(data[:2], b"\0\0")

    @unittest.skipUnless(importlib.util.find_spec("PIL") and importlib.util.find_spec("numpy"), "needs Pillow")
    def test_smooth_screen_keeps_text_out_of_the_overscan(self):
        for width, height in ((1920, 1080), (1280, 720), (720, 720)):
            image = wifi_setup.draw_setup(width, height, wifi_setup.screen_lines("Wignet-6909", "ready"))
            self.assertEqual(image.size, (width, height))
            box = image.convert("L").point(lambda v: 255 if v > 8 else 0).getbbox()
            self.assertGreaterEqual(box[0], width * 0.05)
            self.assertGreaterEqual(box[1], height * 0.05)
            self.assertLessEqual(box[2], width * 0.95)
            self.assertLessEqual(box[3], height * 0.95)
            self.assertEqual(len(wifi_setup.pack_rgb565(image)), width * height * 2)


if __name__ == "__main__":
    unittest.main()
