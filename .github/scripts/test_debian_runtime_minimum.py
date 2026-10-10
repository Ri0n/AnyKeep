#!/usr/bin/env python3
"""Regression tests for lock-derived QCA and Iris Debian runtime minima."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "packaging/debian/runtime-depends.py"
spec = importlib.util.spec_from_file_location("anykeep_runtime_depends", HELPER)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

CONTROLS = (
    "debian/control",
    "packaging/debian/qt6.control",
    "packaging/debian/sdk-ubuntu-24.04.control",
    "packaging/debian/sdk-ubuntu-26.04.control",
    "packaging/debian/sdk-debian-13.control",
)


class RuntimeMinimumTest(unittest.TestCase):
    def test_minimum_versions_come_from_explicit_lock_fields(self):
        lock = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))
        minimums = module.locked_minimums()
        for name in ("qca", "iris"):
            self.assertEqual(minimums[name], lock[name]["minimum_version"])
            self.assertGreaterEqual(module.version_tuple(lock[name]["tag"]),
                                    module.version_tuple(minimums[name]))
        self.assertEqual(
            module.runtime_depends(),
            f"libqca3-qt6-3 (>= {minimums['qca']}), "
            f"libqca3-qt6-plugins (>= {minimums['qca']}), "
            f"libiris-qt6-1 (>= {minimums['iris']})",
        )

    def test_independent_minimums_not_inferred_from_tags(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            lock = {
                "qca": {"tag": "v3.8.9", "minimum_version": "3.0.2"},
                "iris": {"tag": "v1.5.1", "minimum_version": "1.1.0"},
            }
            path.write_text(json.dumps(lock), encoding="utf-8")
            self.assertEqual(
                module.runtime_depends(path),
                "libqca3-qt6-3 (>= 3.0.2), "
                "libqca3-qt6-plugins (>= 3.0.2), "
                "libiris-qt6-1 (>= 1.1.0)",
            )
            lock["iris"]["minimum_version"] = "1.4.0"
            path.write_text(json.dumps(lock), encoding="utf-8")
            self.assertIn("libiris-qt6-1 (>= 1.4.0)", module.runtime_depends(path))
            self.assertIn("libqca3-qt6-3 (>= 3.0.2)", module.runtime_depends(path))

    def test_stable_tag_below_minimum_is_invalid(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            path.write_text(
                json.dumps({
                    "qca": {"tag": "v3.0.9", "minimum_version": "3.0.10"},
                    "iris": {"tag": "v1.1.3", "minimum_version": "1.1.3"},
                }), encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "stable tag"):
                module.locked_minimums(path)

    def test_malformed_or_missing_minimum_is_invalid(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            for minimum in ("latest", "v3.0", "v3.0.0-rc1", ""):
                with self.subTest(minimum=minimum):
                    path.write_text(
                        json.dumps({
                            "qca": {"tag": "v3.2.0", "minimum_version": minimum},
                            "iris": {"tag": "v1.1.3", "minimum_version": "1.1.3"},
                        }), encoding="utf-8"
                    )
                    with self.assertRaises(ValueError):
                        module.locked_minimums(path)

    def test_package_profiles_use_one_substitution(self):
        for filename in CONTROLS:
            with self.subTest(filename=filename):
                content = (ROOT / filename).read_text(encoding="utf-8")
                stanza = content.split("Package: libanykeep3", 1)[1].split(
                    "Package: libanykeep-dev", 1)[0]
                self.assertIn("${runtime:Depends}", stanza)
                self.assertNotIn("libqca3-qt6-3 (>= ", stanza)
                self.assertNotIn("libiris-qt6-1 (>= ", stanza)

    def test_gencontrol_and_ci_read_lock(self):
        rules = (ROOT / "debian/rules").read_text(encoding="utf-8")
        self.assertIn("runtime-depends.py --depends", rules)
        self.assertIn("-Vruntime:Depends=", rules)
        action = (ROOT / ".github/actions/build-deb-package/action.yml").read_text(encoding="utf-8")
        self.assertIn("runtime-depends.py --minimum", action)
        self.assertIn("for provider in qca iris", action)


if __name__ == "__main__":
    unittest.main()
