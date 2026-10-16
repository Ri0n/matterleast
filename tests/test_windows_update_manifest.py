"""Offline verification of Windows release manifest generation."""
import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path

FILE = Path(__file__).resolve().parents[1] / "scripts" / "make-windows-update-manifest.py"
SPEC = importlib.util.spec_from_file_location("manifest", FILE)
MANIFEST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MANIFEST)


class WindowsUpdateManifestTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.asset = Path(temp.name) / "matterleast-1.9-windows-x64-portable.zip"
        self.asset.write_bytes(b"sample zip bytes")

    def test_stable_bytes_and_url(self):
        m = MANIFEST.make_manifest("stable", "v1.9", self.asset)
        self.assertEqual((m["version"], m["releaseTag"]), ("1.9", "v1.9"))
        self.assertEqual(m["package"]["size"], self.asset.stat().st_size)
        self.assertEqual(m["package"]["sha256"], hashlib.sha256(b"sample zip bytes").hexdigest())
        self.assertEqual(m["package"]["url"], "https://github.com/Ri0n/matterleast/releases/download/v1.9/matterleast-1.9-windows-x64-portable.zip")

    def test_nightly_build_identity_is_separate_from_version(self):
        old = MANIFEST.make_manifest("nightly", "nightly-20261016-120000-abc1234", self.asset)
        new = MANIFEST.make_manifest("nightly", "nightly-20261017-010000-abc5678", self.asset)
        self.assertEqual(old["version"], new["version"])
        self.assertLess(old["releaseTag"], new["releaseTag"])

    def test_reject_invalid_tags(self):
        for channel, tag in [
            ("stable", "v1.8"), ("stable", "v1.9/other"),
            ("nightly", "v1.9"), ("nightly", "nightly-latest"),
            ("other", "v1.9")
        ]:
            with self.subTest(channel=channel, tag=tag):
                with self.assertRaises(ValueError):
                    MANIFEST.make_manifest(channel, tag, self.asset)

    def test_reject_accidental_rollback(self):
        old = MANIFEST.make_manifest("stable", "v1.9", self.asset)
        # The same immutable package may be re-published idempotently.
        MANIFEST.validate_promotion(old, old)
        new = dict(old)
        new["version"], new["releaseTag"] = "1.8", "v1.8"
        with self.assertRaises(ValueError):
            MANIFEST.validate_promotion(old, new)
        MANIFEST.validate_promotion(old, new, allow_rollback=True)
        nightly = MANIFEST.make_manifest(
            "nightly", "nightly-20261017-010000-abcdef1", self.asset)
        stale = dict(nightly)
        stale["releaseTag"] = "nightly-20261016-010000-abcdef1"
        with self.assertRaises(ValueError):
            MANIFEST.validate_promotion(nightly, stale)

    def test_reject_invalid_asset(self):
        self.asset.write_bytes(b"")
        with self.assertRaises(ValueError):
            MANIFEST.make_manifest("stable", "v1.9", self.asset)
        self.asset.unlink()
        with self.assertRaises(ValueError):
            MANIFEST.make_manifest("stable", "v1.9", self.asset)


if __name__ == "__main__":
    unittest.main()
