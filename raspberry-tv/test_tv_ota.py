"""The TV's A/B updater against a fake SD card, with real ECDSA signatures."""
import base64
import hashlib
import io
import json
import lzma
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
import tv_ota  # noqa: E402

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
except ImportError:  # the signing side only; the client verifies with libcrypto
    ec = None

MAC = "b8:27:eb:5f:5b:13"


class FakeSd:
    """Slots as files; the boot partition as a dict of what was written."""

    def __init__(self, folder, booted="p2", tryboot=False):
        self.folder = Path(folder)
        self.booted, self.flag, self.up = booted, tryboot, 30.0
        self.reboots, self.boot_installed, self.confirmed = [], [], []
        for slot in tv_ota.SLOTS:
            (self.folder / slot).write_bytes(b"")

    def booted_slot(self):
        return self.booted

    def tryboot(self):
        return self.flag

    def uptime(self):
        return self.up

    def slot_bytes(self, slot):
        return 1024 * 1024

    def open_slot(self, slot):
        return open(self.folder / slot, "r+b", buffering=0)

    def image_hash(self, slot, size):
        return hashlib.sha256((self.folder / slot).read_bytes()[:size]).hexdigest()

    def install_boot(self, slot):
        self.boot_installed.append(slot)

    def confirm(self, slot):
        self.confirmed.append(slot)

    def reboot_tryboot(self):
        self.reboots.append("tryboot")

    def reboot(self):
        self.reboots.append("normal")


@unittest.skipIf(ec is None, "cryptography is needed to sign test offers")
class TvOtaTests(unittest.TestCase):
    def setUp(self):
        self.key = ec.generate_private_key(ec.SECP256R1())
        der = self.key.public_key().public_bytes(serialization.Encoding.DER,
                                                 serialization.PublicFormat.SubjectPublicKeyInfo)
        self.key_id = hashlib.sha256(der).hexdigest()[:8]
        self.keys = {self.key_id: der}
        self.folder = tempfile.TemporaryDirectory()
        self.sd = FakeSd(self.folder.name)
        self.image = b"wignet rootfs " * 4000
        self.sha = hashlib.sha256(self.image).hexdigest()
        self.served = lzma.compress(self.image, format=lzma.FORMAT_XZ)
        self.now = 100.0

    def tearDown(self):
        self.folder.cleanup()

    def sign(self, text):
        return base64.b64encode(self.key.sign(text.encode(), ec.ECDSA(hashes.SHA256()))).decode()

    def ota(self):
        ota = tv_ota.TvOta(MAC, self.folder.name, platform=self.sd, keys=self.keys,
                           clock=lambda: self.now, wall=lambda: 1793000000,
                           opener=lambda url: io.BytesIO(self.served))
        ota.boot()
        return ota

    def offer(self, nonce, **changes):
        offer = {"id": self.sha[:16], "url": f"https://api.wignet.app/firmware/{self.sha}.bin?mac={MAC}",
                 "sha256": self.sha, "size": len(self.image), "hw": "raspberry_pi_hdmi",
                 "counter": 7, "expires": 1793102400, "nonce": nonce, "powerSource": "powered",
                 "powerOverride": False, "key": self.key_id}
        offer.update(changes)
        offer["sig"] = self.sign(tv_ota.offer_canonical(MAC, nonce, offer))
        return offer

    def record(self):
        return json.loads((Path(self.folder.name) / "ota.json").read_text())

    def test_signature_checks_with_libcrypto(self):
        message = "wignet-ota1\nexample"
        signature = self.sign(message)
        self.assertTrue(tv_ota.verify_signature(self.key_id, message, signature, keys=self.keys))
        self.assertFalse(tv_ota.verify_signature(self.key_id, message + "!", signature, keys=self.keys))
        self.assertFalse(tv_ota.verify_signature("00000000", message, signature, keys=self.keys))
        self.assertFalse(tv_ota.verify_signature(self.key_id, message, "not base64!", keys=self.keys))

    def test_production_key_id_matches_the_server(self):
        self.assertEqual(list(tv_ota.KEYS), ["8675e48b"])

    def test_poll_carries_a_fresh_nonce_and_no_report_at_rest(self):
        ota = self.ota()
        first, second = ota.query(), ota.query()
        self.assertRegex(first["on"], r"^[0-9a-f]{32}$")
        self.assertNotEqual(first["on"], second["on"])
        self.assertEqual(set(first), {"on"})

    def test_accepted_offer_writes_the_other_slot_and_reboots_with_tryboot(self):
        ota = self.ota()
        nonce = ota.query()["on"]
        self.assertEqual(ota.answer({"otaOffer": self.offer(nonce)}, 600), 600)
        self.assertEqual((Path(self.folder.name) / "p3").read_bytes(), self.image)
        self.assertEqual(self.sd.boot_installed, ["p3"])
        self.assertEqual(self.sd.reboots, ["tryboot"])
        self.assertEqual(self.record()["phase"], "selected")
        self.assertEqual((self.record()["target"], self.record()["previous"]), ("p3", "p2"))

    def refused(self, nonce_offset=0, advance=0, **changes):
        ota = self.ota()
        nonce = ota.query()["on"]
        offer = self.offer(nonce, **changes)
        self.now += advance
        if nonce_offset:
            offer = self.offer("0" * 32, **changes)
        ota.answer({"otaOffer": offer}, 600)
        return self.sd.reboots == [] and not (Path(self.folder.name) / "p3").read_bytes()

    def test_offers_that_are_not_for_this_poll_or_board_are_ignored(self):
        self.assertTrue(self.refused(nonce_offset=1))  # signed for another poll
        self.assertTrue(self.refused(advance=61))  # answer older than 60 s
        self.assertTrue(self.refused(hw="waveshare_driver_75v2"))
        self.assertTrue(self.refused(url=f"https://evil.example/firmware/{'0' * 64}.bin"))
        self.assertTrue(self.refused(url="https://api.wignet.app.evil.example/firmware/x"))
        self.assertTrue(self.refused(expires=1700000000))
        self.assertTrue(self.refused(size=2 * 1024 * 1024))  # larger than the slot
        self.assertTrue(self.refused(id="0" * 16))

    def test_a_tampered_offer_fails_its_signature(self):
        ota = self.ota()
        nonce = ota.query()["on"]
        offer = self.offer(nonce)
        offer["counter"] = 8
        ota.answer({"otaOffer": offer}, 600)
        self.assertEqual(self.sd.reboots, [])

    def test_counter_must_rise(self):
        Path(self.folder.name, "ota.json").write_text(json.dumps({"counter": 7}))
        self.assertTrue(self.refused())

    def test_a_wrong_hash_fails_and_keeps_the_counter(self):
        self.image = b"something else " * 10
        self.served = lzma.compress(b"not the image! " * 10, format=lzma.FORMAT_XZ)
        self.sha = hashlib.sha256(self.image).hexdigest()
        ota = self.ota()
        ota.answer({"otaOffer": self.offer(ota.query()["on"])}, 600)
        self.assertEqual(self.sd.reboots, [])
        self.assertEqual((self.record()["phase"], self.record()["reason"], self.record()["counter"]),
                         ("failed", "hash", 7))
        report = ota.query()
        self.assertEqual((report["ota"], report["oc"]), (f"failed:{self.sha[:16]}:hash", 7))

    def test_a_truncated_download_fails(self):
        self.served = self.served[:len(self.served) // 2]
        ota = self.ota()
        ota.answer({"otaOffer": self.offer(ota.query()["on"])}, 600)
        self.assertEqual((self.record()["phase"], self.record()["reason"]), ("failed", "download"))

    def selected(self):
        (Path(self.folder.name) / "p3").write_bytes(self.image)
        Path(self.folder.name, "ota.json").write_text(json.dumps(
            {"counter": 7, "sha256": self.sha, "size": len(self.image), "target": "p3",
             "previous": "p2", "phase": "selected"}))

    def test_tryboot_into_the_new_slot_is_pending_until_signed_confirmation(self):
        self.selected()
        self.sd.booted, self.sd.flag = "p3", True
        ota = self.ota()
        self.assertTrue(ota.pending)
        fields = ota.query()
        self.assertEqual((fields["ota"], fields["oh"], fields["oc"]),
                         (f"pending:{self.sha[:16]}", self.sha, 7))
        self.assertEqual(ota.answer({}, 600), 30)  # polls every 30 s while pending
        nonce = ota.query()["on"]
        confirm = {"sha256": self.sha, "counter": 7, "nonce": nonce, "key": self.key_id,
                   "sig": self.sign(tv_ota.confirm_canonical(MAC, self.sha, 7, nonce))}
        self.assertEqual(ota.answer({"otaConfirm": confirm}, 600), 600)
        self.assertEqual(self.sd.confirmed, ["p3"])
        self.assertFalse(ota.pending)
        self.assertEqual(ota.query()["ota"], f"confirmed:{self.sha[:16]}")

    def test_a_confirmation_for_another_poll_is_ignored(self):
        self.selected()
        self.sd.booted, self.sd.flag = "p3", True
        ota = self.ota()
        ota.query()
        other = "1" * 32
        confirm = {"sha256": self.sha, "counter": 7, "nonce": other, "key": self.key_id,
                   "sig": self.sign(tv_ota.confirm_canonical(MAC, self.sha, 7, other))}
        ota.answer({"otaConfirm": confirm}, 600)
        self.assertEqual(self.sd.confirmed, [])
        self.assertTrue(ota.pending)

    def test_unconfirmed_image_reboots_at_the_deadline(self):
        self.selected()
        self.sd.booted, self.sd.flag = "p3", True
        ota = self.ota()
        self.sd.up = tv_ota.PENDING_DEADLINE_SECONDS
        ota.query()
        ota.answer(None, 60)  # the schedule did not even answer
        self.assertEqual(self.sd.reboots, ["normal"])

    def test_back_on_the_previous_slot_reports_a_rollback_twenty_times(self):
        self.selected()
        ota = self.ota()  # booted p2, no tryboot flag
        self.assertFalse(ota.pending)
        reports = [ota.query().get("ota") for _ in range(tv_ota.REPORT_POLLS + 1)]
        self.assertEqual(reports[:-1], [f"rolledback:{self.sha[:16]}"] * tv_ota.REPORT_POLLS)
        self.assertIsNone(reports[-1])

    def test_new_slot_without_tryboot_was_already_confirmed(self):
        self.selected()
        self.sd.booted = "p3"
        self.assertEqual(self.ota().query()["ota"], f"confirmed:{self.sha[:16]}")

    def test_an_interrupted_installation_is_a_failed_write(self):
        Path(self.folder.name, "ota.json").write_text(json.dumps(
            {"counter": 7, "sha256": self.sha, "size": 1, "target": "p3", "previous": "p2",
             "phase": "installing"}))
        self.assertEqual(self.ota().query()["ota"], f"failed:{self.sha[:16]}:write")


class BootConfigTests(unittest.TestCase):
    def test_os_prefix_names_the_slot(self):
        config = "kernel=zImage\nos_prefix=p2/\ndtparam=audio=on\n"
        self.assertEqual(tv_ota.set_os_prefix(config, "p3"), "kernel=zImage\nos_prefix=p3/\ndtparam=audio=on\n")

    def test_a_config_without_os_prefix_is_refused(self):
        with self.assertRaises(tv_ota.Failure):
            tv_ota.set_os_prefix("kernel=zImage\n", "p3")

    def test_only_the_firmware_url_prefix_is_allowed(self):
        self.assertTrue(tv_ota.url_allowed("https://api.wignet.app/firmware/abc.bin?mac=x"))
        self.assertFalse(tv_ota.url_allowed("http://api.wignet.app/firmware/abc.bin"))
        self.assertFalse(tv_ota.url_allowed("https://api.wignet.app:8443/firmware/abc.bin"))
        self.assertFalse(tv_ota.url_allowed("https://api.wignet.app/other/abc.bin"))


if __name__ == "__main__":
    unittest.main()
