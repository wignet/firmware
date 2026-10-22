#!/usr/bin/env python3
"""Small, optional HDMI-CEC helper for the Wignet TV client.

CEC is deliberately outside the display and network paths.  A television that
does not implement CEC, or disables it in its settings, must still show
Wignet normally.  The first CEC message is Image View On: a few television
implementations ignore a wake request when another CEC message precedes it.
"""
from __future__ import annotations

import argparse
import logging
import os
from pathlib import Path
import re
import subprocess


LOG = logging.getLogger("wignet_tv.cec")
CEC_CTL = "/usr/bin/cec-ctl"
DEFAULT_DEVICE = "/dev/cec0"


class CEC:
    """Control a CEC-capable television without making it a client dependency."""

    def __init__(self, enabled: bool, device: str = DEFAULT_DEVICE, runner=subprocess.run):
        self.enabled = enabled
        self.device = device
        self.runner = runner

    @classmethod
    def from_environment(cls):
        value = os.environ.get("WIGNET_CEC", "on").strip().lower()
        if value in ("1", "on", "true", "yes"):
            enabled = True
        elif value in ("0", "off", "false", "no"):
            enabled = False
        else:
            raise ValueError("WIGNET_CEC must be on or off")
        return cls(enabled, os.environ.get("WIGNET_CEC_DEVICE", DEFAULT_DEVICE))

    def wake_tv(self) -> bool:
        """Ask the TV to leave standby and choose this HDMI input.

        A failure is intentionally non-fatal: HDMI video remains the source of
        truth and many monitors do not expose CEC at all.
        """
        if not self.enabled:
            LOG.info("CEC wake disabled")
            return False
        if not Path(self.device).exists():
            LOG.warning("CEC adapter %s is unavailable; continuing without TV wake", self.device)
            return False
        if not Path(CEC_CTL).is_file():
            LOG.warning("cec-ctl is unavailable; install v4l-utils to enable TV wake")
            return False

        configure = [CEC_CTL, "--device", self.device, "--playback", "--osd-name", "Wignet"]
        image_view_on = [CEC_CTL, "--device", self.device, "--to", "0", "--image-view-on"]
        # cec-ctl requires the physical address as the Active Source operand.
        # Raspberry Pi's HDMI connector reports 1.0.0.0 on the supported setup.
        active_source = [CEC_CTL, "--device", self.device, "--to", "0",
                         "--active-source", "phys-addr=1.0.0.0"]
        try:
            # Configure a playback logical address before transmitting.  The
            # wake itself must be the first CEC message sent to the TV.
            for command in (configure, image_view_on, active_source):
                result = self.runner(command, check=False, capture_output=True, text=True, timeout=10)
                if result.returncode:
                    detail = (result.stderr or result.stdout).strip()
                    LOG.warning("CEC command failed (%s): %s", " ".join(command[2:]), detail)
                    return False
        except (OSError, subprocess.TimeoutExpired) as exc:
            LOG.warning("CEC wake failed: %s", exc)
            return False
        LOG.info("CEC Image View On and Active Source sent")
        return True

    def power_status(self):
        """The TV's power as "on" or "standby"; None when it cannot be read.

        Asks the TV (logical address 0) for its power status.  "to-on" and
        "to-standby" count as the state the TV is heading to.
        """
        if not self.enabled or not Path(self.device).exists() or not Path(CEC_CTL).is_file():
            return None
        configure = [CEC_CTL, "--device", self.device, "--playback", "--osd-name", "Wignet"]
        query = [CEC_CTL, "--device", self.device, "--to", "0", "--give-device-power-status"]
        try:
            for command in (configure, query):
                result = self.runner(command, check=False, capture_output=True, text=True, timeout=10)
                if result.returncode:
                    LOG.warning("CEC power status unavailable: %s",
                                (result.stderr or result.stdout).strip())
                    return None
        except (OSError, subprocess.TimeoutExpired) as exc:
            LOG.warning("CEC power status failed: %s", exc)
            return None
        match = re.search(r"pwr-state:\s*(on|standby|to-on|to-standby)", result.stdout or "")
        if match is None:
            return None
        return "on" if match.group(1) in ("on", "to-on") else "standby"

    def standby_tv(self) -> bool:
        """Ask the TV to enter standby; the Raspberry Pi keeps running."""
        if not self.enabled:
            LOG.info("CEC standby disabled")
            return False
        if not Path(self.device).exists():
            LOG.warning("CEC adapter %s is unavailable; cannot put TV in standby", self.device)
            return False
        if not Path(CEC_CTL).is_file():
            LOG.warning("cec-ctl is unavailable; install v4l-utils to control TV standby")
            return False

        command = [CEC_CTL, "--device", self.device, "--to", "0", "--standby"]
        try:
            result = self.runner(command, check=False, capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired) as exc:
            LOG.warning("CEC standby failed: %s", exc)
            return False
        if result.returncode:
            detail = (result.stderr or result.stdout).strip()
            LOG.warning("CEC command failed (--standby): %s", detail)
            return False
        LOG.info("CEC Standby sent; Raspberry Pi remains on")
        return True


def main(argv=None):
    parser = argparse.ArgumentParser(description="Control the TV power state over HDMI-CEC.")
    parser.add_argument("action", choices=("standby", "wake", "status"),
                        help="put the TV in standby, wake/select the Wignet HDMI input, or print its power state")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        cec = CEC.from_environment()
    except ValueError as exc:
        LOG.error("CEC configuration error: %s", exc)
        return 2
    if args.action == "status":
        state = cec.power_status()
        print(state or "unknown")
        return 0 if state else 1
    succeeded = cec.standby_tv() if args.action == "standby" else cec.wake_tv()
    return 0 if succeeded else 1


if __name__ == "__main__":
    raise SystemExit(main())
