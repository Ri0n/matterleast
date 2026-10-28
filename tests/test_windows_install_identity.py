"""Validate the Windows release/rolling-nightly identity contract without Qt."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
POLICY = (ROOT / "cmake/MatterLeastWindowsInstallIdentity.cmake").as_posix()


@unittest.skipUnless(shutil.which("cmake"), "CMake is required")
class WindowsInstallIdentityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="matterleast-build-id-")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        (self.dir / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(identity LANGUAGES NONE)\n"
            f'include("{POLICY}")\n'
            'matterleast_resolve_windows_install_identity("2.0" '
            '"${MATTERLEAST_UPDATE_DEFAULT_CHANNEL}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/resolved.txt" "${MATTERLEAST_INSTALL_ID}")\n',
            encoding="utf-8")

    def configure(self, args=(), success=True):
        import hashlib
        suffix = hashlib.md5(repr(args).encode()).hexdigest()[:9]
        build = self.dir / ("build-" + suffix)
        proc = subprocess.run(
            ["cmake", "-S", str(self.dir), "-B", str(build),
             *args], capture_output=True, text=True, timeout=45)
        if success:
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            return (build / "resolved.txt").read_text(encoding="utf-8")
        self.assertNotEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return proc.stdout + proc.stderr

    def test_stable_uses_numeric_release_identity(self):
        self.assertEqual(
            self.configure(["-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=stable"]), "v2.0")

    def test_nightly_preserves_manifest_build_id(self):
        bid = "nightly-20261028-185307-a743adbc1dfb"
        self.assertEqual(self.configure([
            "-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=nightly",
            "-DMATTERLEAST_INSTALL_ID=" + bid,
        ]), bid)

    def test_ci_build_has_a_unique_id(self):
        ident = "ci-20261028-203000-090bc3c6d046"
        self.assertEqual(self.configure([
            "-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=stable",
            "-DMATTERLEAST_INSTALL_ID=" + ident,
        ]), ident)

    def test_invalid_ci_id_is_rejected(self):
        self.assertIn("Invalid CI", self.configure([
            "-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=stable",
            "-DMATTERLEAST_INSTALL_ID=ci-20261028-203000-a",
        ], False))

    def test_nightly_without_identity_fails(self):
        output = self.configure([
            "-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=nightly",
        ], False)
        self.assertIn("Nightly channel requires a unique", output)

    def test_stable_cannot_impersonate_nightly(self):
        self.assertIn("requires the nightly update channel", self.configure([
            "-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=stable",
            "-DMATTERLEAST_INSTALL_ID=nightly-20261028-185307-a743adbc1dfb",
        ], False))

    def test_invalid_id_is_rejected(self):
        for ident in ("nightly-20261028-185307-a", "../escape",
                      "nightly-20261028-185307-ABCDEF123456",
                      "nightly-20261028-185307-abcdef123456/../"):
            with self.subTest(ident=ident):
                self.assertIn("Invalid", self.configure([
                    "-DMATTERLEAST_UPDATE_DEFAULT_CHANNEL=nightly",
                    "-DMATTERLEAST_INSTALL_ID=" + ident,
                ], False))


if __name__ == "__main__":
    unittest.main()
