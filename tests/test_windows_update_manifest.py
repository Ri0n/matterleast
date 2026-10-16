"""Offline verification of stable and rolling-nightly Windows manifest contracts."""
import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path

FILE = Path(__file__).resolve().parents[1] / "scripts" / "make-windows-update-manifest.py"
SPEC = importlib.util.spec_from_file_location("manifest", FILE)
MANIFEST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MANIFEST)

ID1 = "nightly-20261016-120000-abc1234"
ID2 = "nightly-20261017-010000-abc5678"


class WindowsUpdateManifestTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.asset = Path(temp.name) / "matterleast-1.9-windows-x64-portable.zip"
        self.asset.write_bytes(b"sample zip bytes")

    def nightly_asset(self, build_id=ID1):
        return self.asset.rename(self.asset.with_name(
            f"matterleast-1.9-{build_id}-windows-x64-portable.zip"))

    def test_stable_bytes_and_url(self):
        m = MANIFEST.make_manifest("stable", "v1.9", self.asset)
        self.assertEqual((m["version"], m["releaseTag"]), ("1.9", "v1.9"))
        self.assertNotIn("buildId", m)
        self.assertEqual(m["package"]["size"], self.asset.stat().st_size)
        self.assertEqual(m["package"]["sha256"], hashlib.sha256(b"sample zip bytes").hexdigest())
        self.assertEqual(m["package"]["url"], "https://github.com/Ri0n/matterleast/releases/download/v1.9/matterleast-1.9-windows-x64-portable.zip")

    def test_nightly_uses_one_release_with_unique_asset(self):
        asset = self.nightly_asset()
        m = MANIFEST.make_manifest("nightly", ID1, asset)
        self.assertEqual(m["version"], "1.9")
        self.assertEqual(m["releaseTag"], "updates-nightly")
        self.assertEqual(m["buildId"], ID1)
        self.assertEqual(m["package"]["url"],
                         f"https://github.com/Ri0n/matterleast/releases/download/updates-nightly/{asset.name}")
        self.assertEqual(m["package"]["sha256"], hashlib.sha256(b"sample zip bytes").hexdigest())

    def test_reject_invalid_tags_and_mismatched_assets(self):
        for channel, identity in [
            ("stable", "v1.8"), ("stable", "v1.9/other"),
            ("nightly", "v1.9"), ("nightly", ID2), ("other", "v1.9"),
        ]:
            with self.subTest(channel=channel, identity=identity):
                with self.assertRaises(ValueError):
                    MANIFEST.make_manifest(channel, identity, self.asset)
        asset = self.nightly_asset()
        with self.assertRaises(ValueError):
            MANIFEST.make_manifest("nightly", ID2, asset)
        with self.assertRaises(ValueError):
            MANIFEST.make_manifest("stable", "v1.9", asset)

    def test_stable_rejects_rollback_and_allows_replay(self):
        old = MANIFEST.make_manifest("stable", "v1.9", self.asset)
        MANIFEST.validate_promotion(old, old)
        new = dict(old, version="1.8", releaseTag="v1.8")
        with self.assertRaises(ValueError):
            MANIFEST.validate_promotion(old, new)
        MANIFEST.validate_promotion(old, new, allow_rollback=True)

    def test_nightly_rejects_rollback_in_same_numeric_version(self):
        asset = self.nightly_asset()
        old = MANIFEST.make_manifest("nightly", ID1, asset)
        next_asset = asset.rename(asset.with_name(asset.name.replace(ID1, ID2)))
        new = MANIFEST.make_manifest("nightly", ID2, next_asset)
        MANIFEST.validate_promotion(old, new)
        MANIFEST.validate_promotion(new, new)
        with self.assertRaises(ValueError):
            MANIFEST.validate_promotion(new, old)
        MANIFEST.validate_promotion(new, old, allow_rollback=True)

    def test_migrate_legacy_nightly_pointer_safely(self):
        asset = self.nightly_asset(ID2)
        new = MANIFEST.make_manifest("nightly", ID2, asset)
        legacy = dict(new, releaseTag=ID1)
        del legacy["buildId"]
        MANIFEST.validate_promotion(legacy, new)
        stale = dict(new, releaseTag="updates-nightly", buildId=ID1)
        with self.assertRaises(ValueError):
            MANIFEST.validate_promotion(new, stale)
        with self.assertRaises(ValueError):
            MANIFEST.validate_promotion(dict(new, releaseTag="invalid"), new)

    def test_reject_invalid_asset_or_old_pointer(self):
        self.asset.write_bytes(b"")
        with self.assertRaises(ValueError):
            MANIFEST.make_manifest("stable", "v1.9", self.asset)
        self.asset.unlink()
        with self.assertRaises(ValueError):
            MANIFEST.make_manifest("stable", "v1.9", self.asset)
        self.asset.write_bytes(b"x")
        stable = MANIFEST.make_manifest("stable", "v1.9", self.asset)
        with self.assertRaises(ValueError):
            MANIFEST.validate_promotion(dict(stable, platform="linux-x64"), stable)


if __name__ == "__main__":
    unittest.main()
