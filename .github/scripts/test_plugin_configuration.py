"""Regression coverage for AUTO/ON/OFF plugin selection during CMake configure."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PLUGIN_NAMES = (
    "baseintegration", "cinnamon", "gemini", "gnome", "kdeintegration",
    "macosx", "nextcloud", "openaiwhisper", "spellchecker", "tomboy",
    "yandex", "xmpppubsub",
)


class PluginConfigurationTest(unittest.TestCase):
    def test_plugin_override_and_auto_reconfiguration(self):
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is unavailable")

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source"
            plugins = source / "plugins"
            plugins.mkdir(parents=True)
            (source / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(plugin_config_fixture LANGUAGES CXX)\n"
                "set(ANYKEEP_VERSION_MAJOR 4)\n"
                "set(ANYKEEP_VERSION 4.0.5)\n"
                "set(UNIXLIKE ON)\n"
                f'list(APPEND CMAKE_MODULE_PATH "{(ROOT / "cmake/modules").as_posix()}")\n'
                "include(AnyKeepMacro)\n"
                "add_subdirectory(plugins)\n",
                encoding="utf-8",
            )
            shutil.copyfile(ROOT / "plugins/CMakeLists.txt", plugins / "CMakeLists.txt")
            for name in PLUGIN_NAMES:
                subdir = plugins / name
                subdir.mkdir()
                (subdir / "CMakeLists.txt").write_text(
                    f'file(APPEND "${{CMAKE_BINARY_DIR}}/probes.txt" "{name}\\n")\n'
                    "include(AnyKeepMacro)\n"
                    f'add_anykeep_plugin({name} "fixture" OFF unix)\n',
                    encoding="utf-8",
                )

            build = Path(directory) / "build"
            probes = build / "probes.txt"

            def configure(mode):
                probes.unlink(missing_ok=True)
                result = subprocess.run(
                    [cmake, "-S", str(source), "-B", str(build),
                     f"-DANYKEEP_PLUGIN_ENABLE_spellchecker={mode}"],
                    capture_output=True, text=True,
                )
                present = probes.read_text(encoding="utf-8") if probes.exists() else ""
                return result, present

            off, off_probes = configure("OFF")
            self.assertEqual(off.returncode, 0, off.stdout + off.stderr)
            self.assertNotIn("spellchecker\n", off_probes)
            self.assertIn("PLUGIN spellchecker explicitly disabled", off.stdout)

            on, on_probes = configure("ON")
            self.assertNotEqual(on.returncode, 0)
            self.assertIn("ANYKEEP_PLUGIN_ENABLE_spellchecker=ON", on.stderr)
            self.assertIn("spellchecker\n", on_probes)

            auto, auto_probes = configure("AUTO")
            self.assertEqual(auto.returncode, 0, auto.stdout + auto.stderr)
            self.assertIn("spellchecker\n", auto_probes)
            cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
            self.assertIn("ANYKEEP_PLUGIN_ENABLE_spellchecker:STRING=AUTO", cache)

            again, again_probes = configure("AUTO")
            self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
            self.assertIn("spellchecker\n", again_probes)
            self.assertIn("ANYKEEP_PLUGIN_ENABLE_spellchecker:STRING=AUTO",
                          (build / "CMakeCache.txt").read_text(encoding="utf-8"))

            mobile = subprocess.run(
                [cmake, "-S", str(source), "-B", str(Path(directory) / "mobile"),
                 "-DANYKEEP_BUILD_MOBILE=ON",
                 "-DANYKEEP_PLUGIN_ENABLE_spellchecker=ON"],
                capture_output=True, text=True,
            )
            self.assertNotEqual(mobile.returncode, 0)
            self.assertIn("ANYKEEP_PLUGIN_ENABLE_spellchecker=ON", mobile.stderr)
            self.assertRegex(mobile.stderr, r"not\s+supported by the mobile build")


if __name__ == "__main__":
    unittest.main()
