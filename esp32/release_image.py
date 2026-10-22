"""`pio run -e waveshare_driver_75v2 -t release`: a first-install image.

Offsets and boot images come from PlatformIO, exactly as for USB upload.
Only build outputs are packaged; never read back a device's flash or NVS.
"""
import hashlib
import json
from pathlib import Path
import subprocess

Import("env")  # noqa: F821 - PlatformIO/SCons


def package_release(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    if env["PIOENV"] != "waveshare_driver_75v2":
        raise RuntimeError("Downloadable images are currently validated only for waveshare_driver_75v2")
    if (project / "src/config.h").read_bytes() != (project / "src/config.h.example").read_bytes():
        raise RuntimeError("Release builds require an unchanged copy of src/config.h.example")
    version = subprocess.check_output(
        ["git", "-C", str(project), "rev-parse", "HEAD"], text=True
    ).strip()
    header = (project / "src/firmware_version_gen.h").read_text()
    if f'"{version[:12]}"' not in header:
        raise RuntimeError("Release firmware must come from committed, clean build inputs")
    output = project / ".pio/release"
    output.mkdir(parents=True, exist_ok=True)
    name = f"wignet-{env['PIOENV']}-factory.bin"
    image = output / name
    parts = list(env["FLASH_EXTRA_IMAGES"])
    parts.append((env.subst("$ESP32_APP_OFFSET"), env.subst("$BUILD_DIR/${PROGNAME}.bin")))
    command = [env.subst("$PYTHONEXE"), env.subst("$UPLOADER"), "--chip", "esp32",
               "merge_bin", "--output", str(image)]
    for offset, path in parts:
        command.extend([str(offset), env.subst(str(path))])
    subprocess.run(command, check=True)
    digest = hashlib.sha256(image.read_bytes()).hexdigest()
    (output / "SHA256SUMS").write_text(f"{digest}  {name}\n")
    manifest = {
        "name": "Wignet · Waveshare ESP32 Driver Board · 7.5 V2",
        "version": version[:12],
        "builds": [{"chipFamily": "ESP32", "parts": [{"path": name, "offset": 0}]}],
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "BUILD.json").write_text(json.dumps({
        "repository": "https://github.com/wignet/firmware",
        "commit": version,
        "environment": env["PIOENV"],
        "image": name,
        "offset": 0,
        "sha256": digest,
    }, indent=2) + "\n")
    print(f"Release image: {image} (flash at 0x0)")


env.AddCustomTarget(  # noqa: F821
    name="release",
    dependencies=["$BUILD_DIR/${PROGNAME}.bin"],
    actions=[package_release],
    title="Package first-install firmware",
    description="Create the 7.5-inch factory image, manifest and SHA-256 checksum",
)
