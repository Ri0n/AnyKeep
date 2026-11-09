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


if __name__ == "__main__":
    unittest.main()
