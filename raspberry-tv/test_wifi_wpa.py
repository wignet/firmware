import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).parent))
SPEC = importlib.util.spec_from_file_location("wifi_wpa", Path(__file__).with_name("wifi_wpa.py"))
wifi_wpa = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = wifi_wpa
SPEC.loader.exec_module(wifi_wpa)

SETUP_SPEC = importlib.util.spec_from_file_location(
    "wifi_setup", Path(__file__).with_name("wifi_setup.py"))
wifi_setup = importlib.util.module_from_spec(SETUP_SPEC)
sys.modules[SETUP_SPEC.name] = wifi_setup
SETUP_SPEC.loader.exec_module(wifi_setup)

IW_SCAN = """BSS aa:bb:cc:dd:ee:01(on wlan0)
\tfreq: 2437
\tsignal: -48.00 dBm
\tSSID: Maison:5
\tcapability: ESS Privacy ShortSlotTime (0x0411)
\tRSN:\t * Version: 1
BSS aa:bb:cc:dd:ee:02(on wlan0)
\tsignal: -81.00 dBm
\tSSID: Caf\\xc3\\xa9 libre
\tcapability: ESS (0x0401)
BSS aa:bb:cc:dd:ee:03(on wlan0)
\tsignal: -60.00 dBm
\tSSID:
\tcapability: ESS Privacy (0x0411)
BSS aa:bb:cc:dd:ee:04(on wlan0)
\tsignal: -70.00 dBm
\tSSID: Maison:5
\tcapability: ESS Privacy (0x0411)
\tRSN:\t * Version: 1
"""


class WpaConfigTests(unittest.TestCase):
    def test_psk_is_derived_not_stored(self):
        # IEEE 802.11i-2004, H.4.1: passphrase "password", SSID "IEEE".
        config = wifi_wpa.network_config("IEEE", "password")
        self.assertIn("psk=f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",
                      config)
        self.assertIn("ssid=49454545", config)
        self.assertNotIn("password", config)

    def test_raw_hex_key_and_open_network(self):
        key = "AB" * 32
        self.assertIn("psk=" + key.lower(), wifi_wpa.network_config("x", key))
        self.assertIn("key_mgmt=NONE", wifi_wpa.network_config("Café", ""))

    def test_saved_profile_needs_a_network_block(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(wifi_wpa, "CONF", Path(directory) / "wpa_supplicant.conf"):
            self.assertFalse(wifi_wpa.saved_wifi_profile_exists())
            wifi_wpa.CONF.write_text(wifi_wpa.network_config("IEEE", "password"))
            self.assertTrue(wifi_wpa.saved_wifi_profile_exists())


class IwScanTests(unittest.TestCase):
    def test_scan_feeds_the_portal_list(self):
        points = wifi_setup.parse_access_points(wifi_wpa.parse_iw_scan(IW_SCAN))
        self.assertEqual(points, [
            wifi_setup.AccessPoint("Maison:5", "WPA2", 100),
            wifi_setup.AccessPoint("Café libre", "", 38),
        ])


if __name__ == "__main__":
    unittest.main()
