"""Regression checks for lock-driven Qt desktop/Android toolchain selection."""
import copy
import json
import sys
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qt_versions import qt_configuration


class QtLockTest(unittest.TestCase):
    def setUp(self):
        self.lock = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))

    def test_toolchain_and_keychain_asset_suffix_share_one_version(self):
        qt = qt_configuration(self.lock)
        self.assertEqual(qt["version"], self.lock["qt"]["toolchain_version"])
        self.assertEqual(qt["sdk_suffix"], f'qt{qt["series"]}')
        self.assertTrue(self.lock["qtkeychain"]["tag"].endswith("-" + qt["sdk_suffix"]))

    def test_qt_version_is_not_hardcoded_in_ci(self):
        for filename in (".github/workflows/ci.yml",
                         ".github/workflows/packages.yml",
                         ".github/workflows/deps-qtkeychain.yml"):
            content = (ROOT / filename).read_text(encoding="utf-8")
            with self.subTest(filename=filename):
                self.assertNotIn("QT_CURRENT_SERIES:", content)
                self.assertNotRegex(content, r'QT_SERIES: "[0-9]')
                self.assertNotRegex(content, r"qt6\.[0-9]+-(?:android|windows|macos)")

    def test_bad_toolchain_selector_is_rejected(self):
        for value in ("6.12", "6.12.0", "latest", "6.12.*\nINJECT=yes", ""):
            with self.subTest(value=value):
                data = copy.deepcopy(self.lock)
                data["qt"]["toolchain_version"] = value
                with self.assertRaises(ValueError):
                    qt_configuration(data)

    def test_keychain_from_other_qt_series_is_rejected(self):
        data = copy.deepcopy(self.lock)
        data["qtkeychain"]["tag"] = data["qtkeychain"]["tag"].replace(
            "-qt" + qt_configuration(self.lock)["series"], "-qt6.11")
        with self.assertRaisesRegex(ValueError, "QtKeychain tag"):
            qt_configuration(data)

    def test_minimums_are_independent_but_must_be_supported(self):
        data = copy.deepcopy(self.lock)
        data["qt"]["minimum_mobile"] = "6.99"
        with self.assertRaisesRegex(ValueError, "exceeds"):
            qt_configuration(data)
        data["qt"]["minimum_mobile"] = "6.11"
        self.assertEqual(qt_configuration(data)["version"], self.lock["qt"]["toolchain_version"])


if __name__ == "__main__":
    unittest.main()
