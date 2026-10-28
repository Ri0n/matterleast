"""Dependency lock policy for QCA and QtKeychain.

No network access required; runnable with:
    python3 -m unittest discover -s tests -p test_dependency_lock.py -v
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEMVER = re.compile(r"v?(\d+)\.(\d+)\.(\d+)\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")


class DependencyLockContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lock = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))

    def test_locked_dependencies_have_verified_version_shape(self):
        self.assertEqual(set(self.lock), {"qca", "qtkeychain"})
        for name, entry in self.lock.items():
            with self.subTest(name=name):
                self.assertEqual(set(entry), {"tag", "commit", "minimum_version"})
                self.assertRegex(entry["commit"], SHA)
                tag = SEMVER.fullmatch(entry["tag"])
                minimum = SEMVER.fullmatch(entry["minimum_version"])
                self.assertIsNotNone(tag)
                self.assertIsNotNone(minimum)
                self.assertGreaterEqual(
                    tuple(map(int, tag.groups())),
                    tuple(map(int, minimum.groups())),
                )

    def test_cmake_uses_lock_instead_of_different_hardcoded_revision(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("include(cmake/MatterLeastDependencyLock.cmake)", cmake)
        self.assertIn("matterleast_load_dependency(qca MATTERLEAST_QCA_LOCK)", cmake)
        self.assertNotIn("matterleast_load_dependency(qca_windows", cmake)
        self.assertEqual(self.lock["qca"]["tag"], "v3.1.0")
        self.assertIn("matterleast_load_dependency(qtkeychain MATTERLEAST_QTKEYCHAIN_LOCK)", cmake)
        self.assertIn("tar.gz/\u0024{MATTERLEAST_QCA_LOCK_COMMIT}", cmake)
        self.assertIn("tar.gz/\u0024{MATTERLEAST_QTKEYCHAIN_LOCK_COMMIT}", cmake)
        selection = (ROOT / "cmake/MatterLeastQcaSelection.cmake").read_text(encoding="utf-8")
        self.assertIn("include(cmake/MatterLeastQcaSelection.cmake)", cmake)
        self.assertIn("\u0024{MATTERLEAST_QCA_LOCK_MIN_VERSION} CONFIG QUIET", selection)
        self.assertIn("Qca-qt\u0024{QT_VERSION_MAJOR}", selection)
        for name, entry in self.lock.items():
            self.assertNotIn(entry["commit"], cmake, name)

    def test_bundled_qca_is_fully_static(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        crypto = (ROOT / "sources/backend/LocalMessageCrypto.cpp").read_text(encoding="utf-8")
        tests = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("set(BUILD_SHARED_LIBS OFF CACHE BOOL", cmake)
        self.assertIn("set(LOAD_SHARED_PLUGINS OFF CACHE BOOL", cmake)
        self.assertIn('set(_matterleast_qca_plugin "ossl")', cmake)
        self.assertIn('set(_matterleast_qca_plugin "wincrypto")', cmake)
        self.assertIn('set(BUILD_PLUGINS "${_matterleast_qca_plugin}"', cmake)
        self.assertIn('set(WITH_${_qca_candidate}_PLUGIN "${_qca_enabled}"', cmake)
        for provider in ("botan", "cyrus-sasl", "gcrypt", "gnupg",
                         "logger", "nss", "pkcs11", "softstore"):
            self.assertIn(provider, cmake)
        self.assertIn("foreach(_matterleast_qca_provider IN LISTS PLUGINS)", cmake)
        self.assertIn("Unexpected bundled QCA provider", cmake)
        self.assertIn('set(DEVELOPER_MODE ON CACHE BOOL', cmake)
        self.assertIn('"STATIC_LIBRARY"', cmake)
        self.assertIn("target_link_libraries(matterleast-crypto INTERFACE qca-${_matterleast_qca_plugin})", cmake)
        self.assertIn("QCA_STATIC MATTERLEAST_STATIC_QCA_OSSL", cmake)
        self.assertIn("QCA_STATIC MATTERLEAST_STATIC_QCA_WINCRYPTO", cmake)
        self.assertIn("Q_IMPORT_PLUGIN(qca_ossl)", crypto)
        self.assertIn("Q_IMPORT_PLUGIN(qca_wincrypto)", crypto)
        self.assertIn("matterleast-crypto", tests)
        self.assertNotIn("matterleast-stage-qca-runtime", cmake)

    def test_system_qca_builds_do_not_require_openssl_headers(self):
        ci = (ROOT / ".github/workflows/build.yaml").read_text(encoding="utf-8")
        packages = (ROOT / ".github/workflows/packages.yml").read_text(encoding="utf-8")

        # Only the bundled QCA provider and signed Ed25519 plugin verification
        # compile against OpenSSL headers. System QCA2/3 loads its prebuilt
        # crypto provider dynamically.
        for job, following in (("ubuntu-2404", "ubuntu-2204"),
                               ("ubuntu-2204", "windows")):
            section = ci.split(f"  {job}:", 1)[1].split(f"  {following}:", 1)[0]
            self.assertNotIn("libssl-dev", section, job)
            self.assertIn("libqca-", section, job)

        ubuntu = packages.split("  debian:", 1)[1].split("  debian13:", 1)[0]
        debian = packages.split("  debian13:", 1)[1].split("  appimage:", 1)[0]
        fedora = packages.split("  fedora:", 1)[1].split("  windows:", 1)[0]
        self.assertNotIn("libssl-dev", ubuntu)
        self.assertNotIn("libssl-dev", debian)
        self.assertNotIn("openssl-devel", fedora)
        self.assertIn("qca-qt6-ossl", fedora)
        self.assertIn("libqca-qt6-plugins", debian)

        appimage = packages.split("  appimage:", 1)[1].split("  fedora:", 1)[0]
        self.assertIn("libssl-dev", appimage)
        signing = ci.split("  signed-plugins:", 1)[1]
        self.assertIn("libssl-dev openssl", signing)

    def test_ci_and_release_use_locked_qca(self):
        for path in (
            ROOT / ".github/workflows/build.yaml",
            ROOT / ".github/workflows/packages.yml",
        ):
            with self.subTest(workflow=path.name):
                workflow = path.read_text(encoding="utf-8")
                self.assertIn("-DMATTERLEAST_BUNDLED_QCA=ON", workflow)
                self.assertNotIn(self.lock["qca"]["commit"], workflow)
                self.assertNotIn(self.lock["qca"]["tag"], workflow)
                # Linux packaging still needs OpenSSL for the ossl provider;
                # native Windows packaging must not bundle any OpenSSL DLLs.
                if path.name == "packages.yml":
                    self.assertIn("libssl-dev", workflow)
                self.assertIn("wincrypto", (ROOT / "CMakeLists.txt").read_text())



if __name__ == "__main__":
    unittest.main()
