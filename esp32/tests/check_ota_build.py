"""Check real build artifacts, including the strong Arduino rollback override."""
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ota_package import SLOT_SIZE, validate_image

build = Path(sys.argv[1])
validate_image((build / "firmware.bin").read_bytes())
partitions = {}
raw = (build / "partitions.bin").read_bytes()
for offset in range(0, len(raw) - 31, 32):
    magic, kind, subtype, address, size, label, flags = struct.unpack_from("<HBBII16sI", raw, offset)
    if magic != 0x50AA:
        break
    partitions[label.rstrip(b"\0").decode()] = (kind, subtype, address, size)
assert partitions["app0"] == (0, 0x10, 0x10000, SLOT_SIZE)
assert partitions["app1"] == (0, 0x11, 0x1d0000, SLOT_SIZE)
assert partitions["nvs"] == (1, 2, 0x9000, 0x5000)
assert partitions["otadata"] == (1, 0, 0xe000, 0x2000)
nm = Path.home() / ".platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-nm"
symbols = subprocess.check_output([str(nm), str(build / "firmware.elf")], text=True)
for symbol in ("verifyRollbackLater", "esp_crt_bundle_attach"):
    assert any(line.split()[1:] == ["T", symbol] for line in symbols.splitlines()), symbol
assert any(line.split()[1:] == ["R", "wignet_ota_descriptor"] for line in symbols.splitlines())
print("Actual application, partitions, TLS bundle and strong rollback override verified")
