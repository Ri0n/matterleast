"""Exercise the configure-time update trust contract without Qt/network access."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
POLICY = (ROOT / "cmake/MatterLeastUpdatePolicy.cmake").as_posix()
TEMPLATE = ROOT / "sources/update/UpdateTrustConfig.h.in"


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class UpdatePolicyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="matterleast-update-policy-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "testproject"
        (self.source / "sources/update").mkdir(parents=True)
        (self.source / "sources/update/UpdateTrustConfig.h.in").write_text(
            TEMPLATE.read_text(encoding="utf-8"), encoding="utf-8")
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(update_policy LANGUAGES NONE)\n"
            f'include("{POLICY}")\n', encoding="utf-8")
        self.public_key = self.root / "trusted-public.pem"
        self.public_key.write_text(
            "-----BEGIN PUBLIC KEY-----\nTEST-ONLY-PLACEHOLDER\n"
            "-----END PUBLIC KEY-----\n", encoding="utf-8")

    def cmake(self, options, should_pass=True):
        import hashlib
        key = hashlib.md5(repr(options).encode()).hexdigest()[:12]
        build = self.root / ("build-" + key)
        cmd = ["cmake", "-S", str(self.source), "-B", str(build), *options]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if should_pass:
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            return (build / "generated/UpdateTrustConfig.h").read_text()
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return proc.stdout + proc.stderr

    def test_public_default_uses_github_for_two_channels(self):
        code = self.cmake([])
        self.assertIn("updates-stable/windows-x64.json", code)
        self.assertIn("updates-nightly/windows-x64.json", code)
        self.assertIn("checksCompiled = 1", code)
        self.assertIn("signaturesRequired = 0", code)
        self.assertIn('defaultChannel = "stable"', code)

    def test_configure_logs_resolved_update_urls(self):
        # CMake diagnostics must describe the actual compiled endpoints,
        # not the {channel} template the user passed in.
        for name, options, stable, nightly in (
            ("default", [],
             "https://github.com/Ri0n/matterleast/releases/download/updates-stable/windows-x64.json",
             "https://github.com/Ri0n/matterleast/releases/download/updates-nightly/windows-x64.json"),
            ("corporate", [
                "-DMATTERLEAST_UPDATE_MANIFEST_URL=https://updates.example.test/{channel}/manifest.json"
            ],
             "https://updates.example.test/stable/manifest.json",
             "https://updates.example.test/nightly/manifest.json"),
            ("fixed", [
                "-DMATTERLEAST_UPDATE_MANIFEST_URL=https://updates.example.test/manifest.json"
            ],
             "https://updates.example.test/manifest.json",
             "https://updates.example.test/manifest.json"),
        ):
            with self.subTest(name=name):
                proc = subprocess.run(
                    ["cmake", "-S", str(self.source), "-B", str(self.root / ("build-log-" + name)), *options],
                    capture_output=True, text=True, timeout=30)
                output = proc.stdout + proc.stderr
                self.assertEqual(proc.returncode, 0, output)
                self.assertIn("MatterLeast stable update source: " + stable, output)
                self.assertIn("MatterLeast nightly update source: " + nightly, output)
                self.assertNotIn("MatterLeast update source: ", output)
                self.assertNotIn("{channel}", output)

    def test_nightly_build_defaults_to_nightly(self):
        code = self.cmake(["-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=nightly"])
        self.assertIn('defaultChannel = "nightly"', code)

    def test_invalid_default_channel_is_rejected(self):
        error = self.cmake(["-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=preview"], False)
        self.assertIn("MATTERLEAST_UPDATE_DEFAULT_CHANNEL must be stable or nightly", error)

    def test_custom_url_is_embedded_for_both_channels(self):
        code = self.cmake([
            "-DMATTERLEAST_UPDATE_MANIFEST_URL=https://updates.example.test/{channel}/windows-x64.json"])
        self.assertIn("https://updates.example.test/stable/windows-x64.json", code)
        self.assertIn("https://updates.example.test/nightly/windows-x64.json", code)

    def test_corporate_plugin_key_always_requires_explicit_url(self):
        fail = self.cmake([
            f"-DMATTERLEAST_PLUGIN_VERIFY_KEY_FILE={self.public_key}"], False)
        self.assertIn("MATTERLEAST_UPDATE_MANIFEST_URL", fail)
        code = self.cmake([
            f"-DMATTERLEAST_PLUGIN_VERIFY_KEY_FILE={self.public_key}",
            "-DMATTERLEAST_UPDATE_MANIFEST_URL=https://updates.example.test/{channel}/windows-x64.json"])
        self.assertIn("signaturesRequired = 1", code)

    def test_separate_update_key_requires_url_even_when_checks_disabled(self):
        opts = [f"-DMATTERLEAST_UPDATE_VERIFY_KEY_FILE={self.public_key}"]
        self.assertIn("must explicitly set", self.cmake(
            opts + ["-DMATTERLEAST_ENABLE_UPDATE_CHECKS=OFF"], False))
        code = self.cmake(opts + [
            "-DMATTERLEAST_ENABLE_UPDATE_CHECKS=OFF",
            "-DMATTERLEAST_UPDATE_MANIFEST_URL=https://updates.example.test/stable.json"])
        self.assertIn("checksCompiled = 0", code)
        self.assertIn("#define MATTERLEAST_COMPILED_UPDATE_CHECKS 0", code)
        self.assertIn("signaturesRequired = 0", code)
        self.assertNotIn("TEST-ONLY-PLACEHOLDER", code)
        self.assertIn('stableManifestUrl = R"ml_update_url()ml_update_url"', code)

    def test_unsigned_disable_has_no_endpoint(self):
        code = self.cmake(["-DMATTERLEAST_ENABLE_UPDATE_CHECKS=OFF"])
        self.assertIn("checksCompiled = 0", code)
        self.assertNotIn("github.com", code)

    def test_unsafe_url_rejected_when_update_checks_enabled(self):
        for url in ("http://example.test/a.json", "file:///tmp/a.json",
                    "https://example.test@evil.test/a.json",
                    "https://example.test/a.json;bad",
                    "https://example.test/{channel}/{extra}/file.json"):
            with self.subTest(url=url):
                self.cmake([f"-DMATTERLEAST_UPDATE_MANIFEST_URL={url}"], False)


if __name__ == "__main__":
    unittest.main()
