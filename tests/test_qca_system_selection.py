"""Verify system QCA priority independently of Qt installations/network."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cmake"), "cmake is required")
class SystemQcaSelectionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="matterleast-qca-selection-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.prefix = self.root / "prefix"
        self.source = self.root / "source"
        self.source.mkdir()
        selection = (ROOT / "cmake" / "MatterLeastQcaSelection.cmake").as_posix()
        (self.source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\n"
            "project(qca_selection LANGUAGES NONE)\n"
            "set(QT_VERSION_MAJOR 6)\n"
            "set(MATTERLEAST_QCA_LOCK_MIN_VERSION 3.0.10)\n"
            f'include("{selection}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/selection.txt" '
            '"${MATTERLEAST_QCA_PROVIDER}|${MATTERLEAST_QCA_TARGET}")\n'
        )

    def package(self, package_name, version, target):
        directory = self.prefix / "lib/cmake" / package_name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f"{package_name}Config.cmake").write_text(
            f"add_library({target} INTERFACE IMPORTED)\n"
        )
        (directory / f"{package_name}ConfigVersion.cmake").write_text(
            f'set(PACKAGE_VERSION "{version}")\n'
            "if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)\n"
            "  set(PACKAGE_VERSION_COMPATIBLE FALSE)\n"
            "else()\n"
            "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
            "endif()\n"
        )

    def selected(self, *extra):
        build = self.root / ("build-" + "-".join(x.replace("=", "_") for x in extra))
        cmd = [
            "cmake", "-S", str(self.source), "-B", str(build),
            "-DCMAKE_PREFIX_PATH=" + str(self.prefix), *extra
        ]
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return (build / "selection.txt").read_text()

    def test_qca3_preferred_when_both_installed(self):
        self.package("Qca3-qt6", "3.0.12", "Qca3::Qca")
        self.package("Qca-qt6", "2.3.10", "qca-qt6")
        self.assertEqual(self.selected(), "system-qca3|Qca3::Qca")

    def test_qca2_when_qca3_missing(self):
        self.package("Qca-qt6", "2.3.8", "qca-qt6")
        self.assertEqual(self.selected(), "system-qca2|qca-qt6")

    def test_outdated_qca3_falls_back_to_qca2(self):
        self.package("Qca3-qt6", "3.0.9", "Qca3::Qca")
        self.package("Qca-qt6", "2.3.10", "qca-qt6")
        self.assertEqual(self.selected(), "system-qca2|qca-qt6")

    def test_no_eligible_system_qca_means_bundled_fallback(self):
        self.package("Qca-qt6", "2.3.3", "qca-qt6")
        self.assertEqual(self.selected(), "|")

    def test_explicit_bundled_overrides_system(self):
        self.package("Qca3-qt6", "3.0.12", "Qca3::Qca")
        self.package("Qca-qt6", "2.3.10", "qca-qt6")
        self.assertEqual(self.selected("-DMATTERLEAST_BUNDLED_QCA=ON"), "|")


if __name__ == "__main__":
    unittest.main()
