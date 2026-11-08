"""Keep the Windows Qt toolchain consistent across CI, packaging and upgrade smoke."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"


class WindowsQtToolchainTest(unittest.TestCase):
    def test_windows_qt_versions_agree(self):
        ci = (WORKFLOWS / "build.yaml").read_text(encoding="utf-8")
        packages = (WORKFLOWS / "packages.yml").read_text(encoding="utf-8")
        smoke = (WORKFLOWS / "windows-upgrade-smoke.yml").read_text(encoding="utf-8")

        match = re.search(r'^  QT_WINDOWS_VERSION: "(\d+\.\d+\.\d+)"$',
                          packages, re.MULTILINE)
        self.assertIsNotNone(match, "Windows release Qt version must be pinned")
        version = match.group(1)
        self.assertEqual(version, "6.12.0")
        self.assertIn(f"name: Qt {version} / Windows 2025", ci)
        self.assertIn(f"version: '{version}'", ci)
        self.assertIn(f"key: windows-msvc-qt{version}", ci)
        self.assertIn(f"name: Windows 2025 — NSIS — Qt {version}", packages)
        self.assertIn("version: ${{ env.QT_WINDOWS_VERSION }}", packages)
        self.assertIn(f"key: windows-msvc-qt{version}", packages)
        self.assertIn(f"version: '{version}'", smoke)


if __name__ == "__main__":
    unittest.main()
