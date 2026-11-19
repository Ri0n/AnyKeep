"""Exercise the production CMake version parser, including native package cores."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ReleaseVersionsTest(unittest.TestCase):
    def test_native_core_and_release_identity(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is unavailable")
        module = (Path(__file__).resolve().parents[2] / "cmake/modules/AnyKeepMacro.cmake").read_text()
        parser = module[module.index("macro(sanitize_version"):module.index("endmacro()") + len("endmacro()")]
        cases = [
            ("4.0.0_rc1", "4.0.0", "_rc1", "", ""),
            ("v4.0.0-rc2", "4.0.0", "-rc2", "", ""),
            ("4.0.0_rc1-12-gabcdef", "4.0.0", "_rc1", "12", "abcdef"),
            ("v3.99.0-123-g012abc", "3.99.0", "", "123", "012abc"),
            ("4.0.0", "4.0.0", "", "", ""),
            ("4.0.5\n", "4.0.5", "", "", ""),
            ("  4.0.5 \t\n", "4.0.5", "", "", ""),
            ("3.2", "3.2.0", "", "", ""),
        ]
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "versions.cmake"
            checks = []
            for version, core, suffix, distance, commit in cases:
                checks.append(f'sanitize_version("{version}" APP core)')
                for actual, expected in (("core", core), ("APP_VERSION_SUFFIX", suffix),
                                         ("APP_VERSION_TWEAK", distance), ("APP_VERSION_HASH", commit)):
                    checks.append(f'if(NOT "${{{actual}}}" STREQUAL "{expected}")\n'
                                  f'  message(FATAL_ERROR "{version}: {actual}=${{{actual}}}")\nendif()')
            script.write_text(parser + "\n" + "\n".join(checks))
            result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


    def test_git_release_tag_precedes_stale_source_archive_version(self):
        cmake = shutil.which("cmake")
        git = shutil.which("git")
        if not cmake or not git:
            self.skipTest("CMake and Git are required")
        module = Path(__file__).resolve().parents[2] / "cmake/modules/AnyKeepMacro.cmake"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run([git, "init", "-q", str(root)], check=True)
            (root / "README.md").write_text("fixture")
            subprocess.run([git, "-C", str(root), "add", "README.md"], check=True)
            subprocess.run(
                [git, "-C", str(root), "-c", "user.email=test@example.invalid",
                 "-c", "user.name=Test", "commit", "-qm", "fixture"],
                check=True,
            )
            subprocess.run([git, "-C", str(root), "tag", "v4.0.30"], check=True)
            # A stale archive version must never take precedence over a valid Git tag.
            (root / "anykeep.version").write_text("4.0.5\\n")
            script = root / "version.cmake"
            script.write_text(
                f'include("{module.as_posix()}")\\n'
                'if(NOT ANYKEEP_VERSION STREQUAL "4.0.30")\\n'
                '  message(FATAL_ERROR "Git release version was not used: ${ANYKEEP_VERSION}")\\n'
                'endif()\\n'
            )
            result = subprocess.run([cmake, "-P", str(script)], cwd=root,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_git_free_archive_uses_generated_version(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is required")
        module = Path(__file__).resolve().parents[2] / "cmake/modules/AnyKeepMacro.cmake"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "anykeep.version").write_text("4.0.30\\n")
            (root / "debian").mkdir()
            (root / "debian/changelog").write_text("anykeep (4.0.5) unstable; urgency=low\\n")
            script = root / "version.cmake"
            script.write_text(
                f'include("{module.as_posix()}")\\n'
                'if(NOT ANYKEEP_VERSION STREQUAL "4.0.30")\\n'
                '  message(FATAL_ERROR "Archive version was not used: ${ANYKEEP_VERSION}")\\n'
                'endif()\\n'
            )
            result = subprocess.run([cmake, "-P", str(script)], cwd=root,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
