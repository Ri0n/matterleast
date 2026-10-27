"""Dependency lock policy for QCA and QtKeychain.

No network access required; runnable with:
    python3 -m unittest discover -s tests -p test_dependency_lock.py -v
"""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEMVER = re.compile(r"v?(\\d+)\\.(\\d+)\\.(\\d+)\\Z")
SHA = re.compile(r"[0-9a-f]{40}\\Z")


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
        self.assertIn("matterleast_load_dependency(qtkeychain MATTERLEAST_QTKEYCHAIN_LOCK)", cmake)
        self.assertIn("tar.gz/\u0024{MATTERLEAST_QCA_LOCK_COMMIT}", cmake)
        self.assertIn("tar.gz/\u0024{MATTERLEAST_QTKEYCHAIN_LOCK_COMMIT}", cmake)
        self.assertIn("\u0024{MATTERLEAST_QCA_LOCK_MIN_VERSION} CONFIG QUIET", cmake)
        for name, entry in self.lock.items():
            self.assertNotIn(entry["commit"], cmake, name)

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
                self.assertIn("libssl-dev", workflow if path.name == "build.yaml" else
                              workflow)  # OpenSSL headers are distinct from QCA sources


if __name__ == "__main__":
    unittest.main()
