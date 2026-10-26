import os
import re

Import("env")


EXPECTED_DRIVERS = {
    "waveshare_photopainter_73e6": {"GxEPD2_730c_GDEP073E01"},
    "waveshare_driver_75v2": {"GxEPD2_750_T7"},
    "waveshare_driver_75v2_ota": {"GxEPD2_750_T7"},
    "waveshare_driver_75v2_ota_reject": {"GxEPD2_750_T7"},
    "waveshare_driver_75v2_clear_wifi": {"GxEPD2_750_T7"},
    "waveshare_driver_75v2_no_default_wifi": {"GxEPD2_750_T7"},
    "firebeetle_75v2": {"GxEPD2_750_T7"},
    "d1_mini_75v2": {"GxEPD2_750_T7"},
    "d1_mini_35": set(),
    "waveshare_driver_27": {"GxEPD2_270"},
    "waveshare_driver_35": set(),
    "waveshare_esp32s3_lcd_154": set(),
    "waveshare_esp32s3_lcd_2": set(),
}

CORE_OBJECTS = {"GxEPD2_EPD"}
GXEPD2_OBJECT_RE = re.compile(r"libGxEPD2\.a\((GxEPD2_[^)]+)\.cpp\.o\)")


def verify_linked_gxepd2_drivers(source, target, env):
    pioenv = env.subst("$PIOENV")
    expected = EXPECTED_DRIVERS.get(pioenv)
    if expected is None:
        print(f"[GxEPD2 Check] No expected-driver rule for {pioenv}; skipping")
        return

    map_path = os.path.join(env.subst("$BUILD_DIR"), f"{env.subst('$PROGNAME')}.map")
    if not os.path.exists(map_path):
        print(f"[GxEPD2 Check] Map file not found: {map_path}")
        return

    with open(map_path, "r", encoding="utf-8", errors="ignore") as map_file:
        linked_objects = set(GXEPD2_OBJECT_RE.findall(map_file.read()))

    linked_drivers = linked_objects - CORE_OBJECTS
    unexpected = linked_drivers - expected
    missing = expected - linked_drivers

    if unexpected or missing:
        print("[GxEPD2 Check] Unexpected linked display drivers")
        print(f"  expected: {sorted(expected)}")
        print(f"  linked:   {sorted(linked_drivers)}")
        env.Exit(1)

    if linked_drivers:
        print(f"[GxEPD2 Check] Linked driver(s): {', '.join(sorted(linked_drivers))}")
    else:
        print("[GxEPD2 Check] No GxEPD2 display driver linked")


env.AddPostAction(
    "$BUILD_DIR/${PROGNAME}.elf",
    env.VerboseAction(verify_linked_gxepd2_drivers, "Checking linked GxEPD2 drivers"),
)
