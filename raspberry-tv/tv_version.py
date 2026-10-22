#!/usr/bin/env python3
"""Print the version the TV client reports as `fw=` on /schedule.

Same format as esp32/firmware_version.py: the short commit of HEAD,
plus `-dirty.<hash>` when an installed file differs from it.  install.sh and
the Buildroot package write the output to VERSION next to wignet_tv.py;
a tree without git gives `unknown`.
"""

import hashlib
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# What install.sh copies to the Pi (tests and Buildroot recipes are not code
# the Pi runs).
INPUTS = ("wignet_tv.py", "wifi_setup.py", "wifi_wpa.py", "cec.py", "tv_power.py",
          "sound_list.py", "sound_player.py", "tune_render.py", "boot-splash.sh",
          "wignet-tv-boot.sh", "../esp32/portal")


def _git(*args):
    return subprocess.run(["git", "-c", "safe.directory=*", "-C", str(HERE), *args],
                          check=True, capture_output=True, text=True).stdout


def version():
    try:
        commit = _git("rev-parse", "--short=12", "HEAD").strip()
        diff = _git("diff", "HEAD", "--binary", "--", *INPUTS)
        untracked = _git("ls-files", "--others", "--exclude-standard", "--", *INPUTS).split()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    if not diff and not untracked:
        return commit
    digest = hashlib.sha256(diff.encode())
    for name in sorted(untracked):
        digest.update(name.encode())
        digest.update((HERE / name).read_bytes())
    return f"{commit}-dirty.{digest.hexdigest()[:8]}"


if __name__ == "__main__":
    sys.stdout.write(version() + "\n")
