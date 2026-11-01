"""Verify that optional ISO overlays stay bounded and preserve other bytes."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from disc_overlay import DiscOverlay, sha256  # noqa: E402


class DiscOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.disc = self.root / "disc.iso"
        self.disc.write_bytes(b"0123456789abcdef")
        (self.root / "patch.bin").write_bytes(b"ABCD")
        self.manifest = self.root / "overlay.json"
        self.data = {"format": "iso-byte-overlay-v1", "disc_size": 16,
                     "patches": [{"offset": 5, "length": 4,
                                  "original_sha256": sha256(b"5678"),
                                  "data_file": "patch.bin"}]}
        self.write_manifest()

    def write_manifest(self):
        self.manifest.write_text(json.dumps(self.data), encoding="utf-8")

    def test_full_and_partial_ranges(self):
        overlay = DiscOverlay(self.manifest, self.disc)
        self.assertEqual(overlay.apply(0, self.disc.read_bytes()), b"01234ABCD9abcdef")
        self.assertEqual(overlay.apply(3, b"3456789a"), b"34ABCD9a")
        self.assertEqual(overlay.apply(7, b"789a"), b"CD9a")
        self.assertEqual(overlay.apply(9, b"9abcd"), b"9abcd")
        self.assertEqual(self.disc.read_bytes(), b"0123456789abcdef")

    def test_rejects_mismatched_source(self):
        self.data["patches"][0]["original_sha256"] = sha256(b"wrong")
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "do not match"):
            DiscOverlay(self.manifest, self.disc)

    def test_rejects_payload_outside_manifest_directory(self):
        self.data["patches"][0]["data_file"] = "../patch.bin"
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "local filename"):
            DiscOverlay(self.manifest, self.disc)

    def test_multiple_assets_and_rejects_overlap(self):
        (self.root / "second.bin").write_bytes(b"XY")
        second = self.root / "second.json"
        second_data = {"format": "iso-byte-overlay-v1", "disc_size": 16,
                       "patches": [{"offset": 12, "length": 2,
                                    "original_sha256": sha256(b"cd"),
                                    "data_file": "second.bin"}]}
        second.write_text(json.dumps(second_data), encoding="utf-8")
        overlay = DiscOverlay([self.manifest, second], self.disc)
        self.assertEqual(overlay.apply(0, self.disc.read_bytes()), b"01234ABCD9abXYef")
        second_data["patches"][0]["offset"] = 7
        second_data["patches"][0]["original_sha256"] = sha256(b"78")
        second.write_text(json.dumps(second_data), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "overlap"):
            DiscOverlay([self.manifest, second], self.disc)


if __name__ == "__main__":
    unittest.main()
