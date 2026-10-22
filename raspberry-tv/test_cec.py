import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).parent))
import cec


class Result:
    returncode = 0
    stdout = ""
    stderr = ""


class CECTests(unittest.TestCase):
    def test_wake_configures_then_wakes_then_selects_source(self):
        commands = []

        def runner(command, **_):
            commands.append(command)
            return Result()

        with patch.object(cec.Path, "exists", return_value=True), \
             patch.object(cec.Path, "is_file", return_value=True):
            self.assertTrue(cec.CEC(True, "/dev/cec0", runner).wake_tv())

        self.assertEqual(commands[0][-3:], ["--playback", "--osd-name", "Wignet"])
        self.assertEqual(commands[1][-3:], ["--to", "0", "--image-view-on"])
        self.assertEqual(commands[2][-4:], ["--to", "0", "--active-source", "phys-addr=1.0.0.0"])

    def test_standby_sends_standby_to_the_tv_without_powering_down_the_pi(self):
        commands = []

        def runner(command, **_):
            commands.append(command)
            return Result()

        with patch.object(cec.Path, "exists", return_value=True), \
             patch.object(cec.Path, "is_file", return_value=True):
            self.assertTrue(cec.CEC(True, "/dev/cec0", runner).standby_tv())

        self.assertEqual(commands, [[cec.CEC_CTL, "--device", "/dev/cec0", "--to", "0", "--standby"]])

    def test_standby_respects_disabled_cec(self):
        self.assertFalse(cec.CEC(False).standby_tv())

    def test_standby_sends_standby_to_the_tv_without_powering_down_the_pi(self):
        commands = []

        def runner(command, **_):
            commands.append(command)
            return Result()

        with patch.object(cec.Path, "exists", return_value=True), \
             patch.object(cec.Path, "is_file", return_value=True):
            self.assertTrue(cec.CEC(True, "/dev/cec0", runner).standby_tv())

        self.assertEqual(commands, [[cec.CEC_CTL, "--device", "/dev/cec0", "--to", "0", "--standby"]])

    def test_standby_respects_disabled_cec(self):
        self.assertFalse(cec.CEC(False).standby_tv())

    def test_missing_adapter_is_non_fatal(self):
        with patch.object(cec.Path, "exists", return_value=False):
            self.assertFalse(cec.CEC(True).wake_tv())

    def test_environment_can_disable_cec(self):
        with patch.dict(os.environ, {"WIGNET_CEC": "off"}, clear=False):
            self.assertFalse(cec.CEC.from_environment().enabled)


if __name__ == "__main__":
    unittest.main()
