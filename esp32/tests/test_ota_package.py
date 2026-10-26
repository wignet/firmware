import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_package as ota


class ArtifactTests(unittest.TestCase):
    def image(self):
        data = bytearray(512)
        data[0:2] = b"\xe9\x01"
        struct.pack_into("<I", data, 28, 480)
        struct.pack_into("<I", data, 32, 0xABCD5432)
        data[ota.DESCRIPTOR_OFFSET:ota.DESCRIPTOR_OFFSET + len(ota.DESCRIPTOR)] = ota.DESCRIPTOR
        return data

    def test_factory_other_board_layout_and_truncation_rejected(self):
        data = self.image()
        for bad in (data[:367], b"\xff" * 4096, data + bytes(ota.SLOT_SIZE),
                    bytearray(data)):
            if bad == data:
                bad[ota.DESCRIPTOR_OFFSET + 16] ^= 1
            with self.assertRaises(ValueError):
                ota.validate_image(bad)
        data[ota.DESCRIPTOR_OFFSET + 64] ^= 1
        with self.assertRaises(ValueError):
            ota.validate_image(data)

    def test_manifest_describes_exact_application(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "app.bin"
            image.write_bytes(self.image())
            manifest = ota.package(image, root / "out", "waveshare_driver_75v2_ota", "test")
            artifact = (root / "out/firmware-ota.bin").read_bytes()
            self.assertEqual(artifact, image.read_bytes())
            self.assertEqual(manifest["sha256"], hashlib.sha256(artifact).hexdigest())
            self.assertEqual(manifest["size"], len(artifact))
            self.assertEqual(manifest, json.loads((root / "out/ota.json").read_text()))
            self.assertFalse(manifest["fault_injection"])
            self.assertTrue(manifest["experimental"])

    def test_partition_layout_keeps_nvs_and_fits_flash(self):
        entries = []
        for line in (Path(__file__).resolve().parents[1] / "partitions/ota_4m.csv").read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            name, kind, subtype, offset, size, *_ = [v.strip() for v in line.split(",")]
            entries.append((name, int(offset, 0), int(size, 0)))
        self.assertEqual(entries[0], ("nvs", 0x9000, 0x5000))
        self.assertEqual(entries[1], ("otadata", 0xe000, 0x2000))
        self.assertEqual(entries[2], ("app0", 0x10000, ota.SLOT_SIZE))
        self.assertEqual(entries[3], ("app1", 0x1d0000, ota.SLOT_SIZE))
        for (_, offset, size), (_, following, _) in zip(entries, entries[1:]):
            self.assertEqual(offset + size, following)
        self.assertEqual(entries[-1][1] + entries[-1][2], 4 * 1024 * 1024)


if __name__ == "__main__":
    unittest.main()
