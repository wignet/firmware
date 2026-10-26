"""Validate/package application-only OTA bench artifacts; also usable as a CLI."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import struct

TARGET = "waveshare_driver_75v2"
LAYOUT = "ota-4m-v1"
SLOT_SIZE = 0x1C0000
DESCRIPTOR_OFFSET = 24 + 8 + 256
DESCRIPTOR = struct.pack("16s48s16s", b"WIGNET_OTA_V1", TARGET.encode(), LAYOUT.encode())


def validate_image(data: bytes) -> None:
    if not DESCRIPTOR_OFFSET + len(DESCRIPTOR) <= len(data) <= SLOT_SIZE:
        raise ValueError("application size outside OTA slot budget")
    if data[0] != 0xE9 or data[1] == 0 or struct.unpack_from("<H", data, 12)[0] != 0:
        raise ValueError("not an ESP32 application")
    if struct.unpack_from("<I", data, 28)[0] < 256 + len(DESCRIPTOR):
        raise ValueError("first segment is too short")
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError("missing ESP application descriptor (factory image?)")
    if data[DESCRIPTOR_OFFSET:DESCRIPTOR_OFFSET + len(DESCRIPTOR)] != DESCRIPTOR:
        raise ValueError("wrong board, partition layout or missing OTA contract")


def package(image: Path, output: Path, environment: str, version: str) -> dict:
    data = image.read_bytes()
    validate_image(data)
    manifest = {
        "schema": "wignet-ota-bench-v1",
        "experimental": True,
        "target": TARGET,
        "layout": LAYOUT,
        "environment": environment,
        "version": version,
        "fault_injection": environment.endswith("_reject"),
        "image": "firmware-ota.bin",
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / manifest["image"]).write_bytes(data)
    (output / "ota.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"OTA bench: {len(data)} / {SLOT_SIZE} bytes; {SLOT_SIZE - len(data)} bytes spare")
    return manifest


try:
    Import("env")  # noqa: F821 - PlatformIO/SCons
except NameError:
    if __name__ == "__main__":
        import argparse
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("image", type=Path)
        parser.add_argument("output", type=Path)
        args = parser.parse_args()
        package(args.image, args.output, "manual", "unknown")
else:
    def after_build(source, target, env):
        project = Path(env.subst("$PROJECT_DIR"))
        if (project / "src/config.h").read_bytes() != (project / "src/config.h.example").read_bytes():
            raise RuntimeError("OTA bench artifacts require src/config.h.example")
        header = (project / "src/firmware_version_gen.h").read_text()
        version = header.split('#define WIGNET_FW_VERSION "', 1)[1].split('"', 1)[0]
        package(Path(env.subst("$BUILD_DIR/${PROGNAME}.bin")),
                project / ".pio/ota" / env["PIOENV"], env["PIOENV"], version)

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)  # noqa: F821
