"""`pio run -e <environment> -t release`: a first-install image.

Offsets and boot images come from PlatformIO, exactly as for USB upload.
Only build outputs are packaged; never read back a device's flash or NVS.

With WIGNET_RELEASE_APP=1 (the workflow's list of OTA boards), also the
application alone, `wignet-<environment>-app.bin`, which the server imports
from a published release and installs over the air (docs/ota.md, wignet).
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

Import("env")  # noqa: F821 - PlatformIO/SCons

TARGETS = {
    "waveshare_driver_75v2": ("Waveshare ESP32 Driver Board · 7.5 V2", "esp32", "ESP32"),
    "waveshare_photopainter_73e6": ("Waveshare PhotoPainter · 7.3 Spectra 6", "esp32s3", "ESP32-S3"),
}


def empty_littlefs(env, build):
    """Initialize only the factory image, using its compiled partition table."""
    table = (build / "partitions.bin").read_bytes()
    partition = None
    for at in range(0, len(table) - 31, 32):
        magic, kind, subtype, offset, size, label, flags = struct.unpack_from("<HBBII16sI", table, at)
        if magic != 0x50AA:
            break
        if kind == 1 and subtype == 0x82 and label.rstrip(b"\0") == b"spiffs":
            partition = offset, size
            break
    if partition is None:
        raise RuntimeError("LittleFS partition not found")
    offset, size = partition
    tool_directory = env.PioPlatform().get_package_dir("tool-mklittlefs")
    if tool_directory is None:
        raise RuntimeError("PhotoPainter release requires the tool-mklittlefs package")
    tool = Path(tool_directory) / (
        "mklittlefs.exe" if os.name == "nt" else "mklittlefs"
    )
    image = build / "release-littlefs.bin"
    with tempfile.TemporaryDirectory(prefix="wignet-release-empty-") as directory:
        subprocess.run([str(tool), "-c", directory, "-b", "4096", "-p", "256",
                        "-s", str(size), str(image)], check=True)
    return offset, size, image


def package_release(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    if env["PIOENV"] not in TARGETS:
        raise RuntimeError("This board has no validated downloadable image")
    title, chip, family = TARGETS[env["PIOENV"]]
    if (project / "src/config.h").read_bytes() != (project / "src/config.h.example").read_bytes():
        raise RuntimeError("Release builds require an unchanged copy of src/config.h.example")
    version = subprocess.check_output(
        ["git", "-C", str(project), "rev-parse", "HEAD"], text=True
    ).strip()
    header = (project / "src/firmware_version_gen.h").read_text()
    if f'"{version[:12]}"' not in header:
        raise RuntimeError("Release firmware must come from committed, clean build inputs")
    output = project / ".pio/release" / env["PIOENV"]
    output.mkdir(parents=True, exist_ok=True)
    prefix = f"wignet-{env['PIOENV']}"
    name = f"{prefix}-factory.bin"
    image = output / name
    parts = list(env["FLASH_EXTRA_IMAGES"])
    parts.append((env.subst("$ESP32_APP_OFFSET"), env.subst("$BUILD_DIR/${PROGNAME}.bin")))
    filesystem = None
    if chip == "esp32s3":
        offset, size, fs_image = empty_littlefs(env, Path(env.subst("$BUILD_DIR")))
        parts.append((hex(offset), str(fs_image)))
        filesystem = {"offset": offset, "size": size, "content": "empty LittleFS"}
    command = [env.subst("$PYTHONEXE"), env.subst("$UPLOADER"), "--chip", chip,
               "merge_bin", "--output", str(image)]
    for offset, path in parts:
        command.extend([str(offset), env.subst(str(path))])
    subprocess.run(command, check=True)
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    sums = f"{digest}  {name}\n"
    if os.environ.get("WIGNET_RELEASE_APP") == "1":
        app = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin")).read_bytes()
        # The server installs only an image that declares ota1 itself, found
        # by this literal: one that did not would strand its board. The 7.5 V2
        # declares it alone, so first (`v1%3Aota1`).
        if b"%2Cota1" not in app and b"v1%3Aota1" not in app:
            raise RuntimeError("An over-the-air image must declare ota1 (WIGNET_OTA_CAPABLE)")
        app_name = f"{prefix}-app.bin"
        (output / app_name).write_bytes(app)
        sums += f"{hashlib.sha256(app).hexdigest()}  {app_name}\n"
    (output / f"{prefix}-SHA256SUMS").write_text(sums)
    manifest = {
        "name": f"Wignet · {title}",
        "version": version[:12],
        "builds": [{"chipFamily": family, "parts": [{"path": name, "offset": 0}]}],
    }
    (output / f"{prefix}-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / f"{prefix}-BUILD.json").write_text(json.dumps({
        "repository": "https://github.com/wignet/firmware",
        "commit": version,
        "environment": env["PIOENV"],
        "image": name,
        "offset": 0,
        "sha256": digest,
        "filesystem": filesystem,
    }, indent=2) + "\n")
    print(f"Release image: {image} (flash at 0x0)")


env.AddCustomTarget(  # noqa: F821
    name="release",
    dependencies=["$BUILD_DIR/${PROGNAME}.bin"],
    actions=[package_release],
    title="Package first-install firmware",
    description="Create the board's factory image, manifest and SHA-256 checksum",
)
