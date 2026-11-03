"""Whether firmware build inputs changed for one release target.

Used by the release workflow to avoid building an unchanged board. Shared
ESP32 sources rebuild both production ESP32 targets; a board's HAL source and
partition table rebuild only that target.
"""
from __future__ import annotations

import argparse
import subprocess


TARGETS = {
    "driver-75v2": {
        "environment": "waveshare_driver_75v2",
        "hal": "esp32/src/hal/waveshare_driver_75v2.cpp",
        "partitions": {"esp32/partitions/ota_4m.csv"},
    },
    "photopainter-73e6": {
        "environment": "waveshare_photopainter_73e6",
        "hal": "esp32/src/hal/waveshare_photopainter_73e6.cpp",
        "partitions": set(),
    },
    "amoled-18": {
        "environment": "waveshare_esp32s3_amoled_18",
        "hal": "esp32/src/hal/waveshare_esp32s3_amoled_18.cpp",
        "partitions": set(),
    },
    "tv-pi0w": {"environment": None, "hal": None, "partitions": set()},
}

ESP32_SHARED_FILES = {
    "esp32/embed_portal.py",
    "esp32/optimize_gxepd2.py",
    "esp32/firmware_version.py",
    "esp32/release_image.py",
}
TV_RUNTIME_FILES = {
    "raspberry-tv/wignet_tv.py",
    "raspberry-tv/wifi_setup.py",
    "raspberry-tv/cec.py",
    "raspberry-tv/tv_power.py",
    "raspberry-tv/boot-splash.sh",
    "raspberry-tv/wifi_wpa.py",
    "raspberry-tv/sound_list.py",
    "raspberry-tv/sound_player.py",
    "raspberry-tv/tune_render.py",
    "raspberry-tv/mml.py",
    "raspberry-tv/tv_ota.py",
    "raspberry-tv/tv_version.py",
}


def _git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def _sections(text: str, environment: str) -> tuple[str, ...]:
    wanted = {"platformio", "env", f"env:{environment}"}
    current = None
    collected: dict[str, list[str]] = {name: [] for name in wanted}
    for line in text.splitlines():
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1]
        if current in collected:
            collected[current].append(line.rstrip())
    return tuple("\n".join(collected[name]) for name in ("platformio", "env", f"env:{environment}"))


def _changed_files(base: str, head: str) -> set[str]:
    return set(_git("diff", "--name-only", f"{base}...{head}").splitlines())


def target_inputs_changed(target_name: str, base: str, head: str) -> bool:
    target = TARGETS[target_name]
    files = _changed_files(base, head)
    if target_name == "tv-pi0w":
        return any(path.startswith(("raspberry-tv/buildroot/", "esp32/portal/"))
                   or path in TV_RUNTIME_FILES for path in files)

    for path in files:
        if path.startswith(("esp32/src/", "esp32/portal/")):
            if path.startswith("esp32/src/hal/waveshare_") and path.endswith(".cpp") and path != target["hal"]:
                continue
            return True
        if path in ESP32_SHARED_FILES or path in target["partitions"]:
            return True
        if path.startswith("esp32/partitions/"):
            continue
        if path == "esp32/platformio.ini":
            before = _git("show", f"{base}:{path}")
            after = _git("show", f"{head}:{path}")
            if _sections(before, target["environment"]) != _sections(after, target["environment"]):
                return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("target", choices=TARGETS)
    parser.add_argument("base")
    parser.add_argument("head")
    args = parser.parse_args()
    print("changed" if target_inputs_changed(args.target, args.base, args.head) else "unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
