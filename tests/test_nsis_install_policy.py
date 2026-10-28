"""Check per-user NSIS install defaults against the installed CMake template."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cmake"), "CMake unavailable")
class PerUserNsisTemplateTest(unittest.TestCase):
    def test_template_does_not_fallback_to_documents_or_elevation(self):
        with tempfile.TemporaryDirectory() as temp:
            target = pathlib.Path(temp)
            script = target / "test.cmake"
            cmake_module = (ROOT / "cmake/MatterLeastPerUserNsis.cmake").as_posix()
            script.write_text(
                f'include("{cmake_module}")\n'
                f'matterleast_prepare_per_user_nsis_template("{target.as_posix()}")\n',
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cmake", "-P", str(script)], capture_output=True, text=True, timeout=30
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            template = (target / "NSIS.template.in").read_text(encoding="utf-8")
            self.assertEqual(template.count("RequestExecutionLevel user"), 1)
            self.assertNotIn("RequestExecutionLevel admin", template)
            self.assertNotIn('StrCpy $INSTDIR "$DOCUMENTS', template)
            self.assertNotIn("SetShellVarContext all", template)
            self.assertIn('InstallDir "@CPACK_NSIS_INSTALL_ROOT@\\@CPACK_PACKAGE_INSTALL_DIRECTORY@"', template)
            self.assertIn("SetShellVarContext current", template)
            self.assertRegex(template, r'Function un\.onInit\s+SetShellVarContext current\s+FunctionEnd')
            self.assertNotIn('HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@', template)
            self.assertIn('HKCU "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\@CPACK_PACKAGE_INSTALL_REGISTRY_KEY@', template)

    def test_project_selects_local_programs_and_custom_cpack_module_path(self):
        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('set(CPACK_NSIS_INSTALL_ROOT "$LOCALAPPDATA\\\\\\\\Programs")', root_cmake)
        self.assertIn('set(CPACK_MODULE_PATH "${_matterleast_nsis_templates}")', root_cmake)
        self.assertIn('matterleast_prepare_per_user_nsis_template(', root_cmake)
        self.assertIn('set(CPACK_COMPONENT_DEVELOPMENT_DISABLED TRUE)', root_cmake)
        self.assertNotIn('WindowsDeployOpenSSL.cmake.in', root_cmake)
        self.assertNotIn('"RequestExecutionLevel user\\n"', root_cmake)


if __name__ == "__main__":
    unittest.main()
