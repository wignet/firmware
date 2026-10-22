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


if __name__ == "__main__":
    unittest.main()
