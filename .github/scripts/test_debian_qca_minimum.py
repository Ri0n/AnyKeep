#!/usr/bin/env python3
"""Debian QCA version floors must come only from dependencies.lock.json."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "packaging/debian/qca-depends.py"
spec = importlib.util.spec_from_file_location("anykeep_qca_depends", HELPER)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

CONTROLS = (
    "debian/control",
    "packaging/debian/qt6.control",
    "packaging/debian/sdk-ubuntu-24.04.control",
    "packaging/debian/sdk-ubuntu-26.04.control",
    "packaging/debian/sdk-debian-13.control",
)


class DebianQcaMinimumTest(unittest.TestCase):
    def test_minimum_is_derived_from_stable_qca_tag(self):
        lock = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))
        version = lock["qca"]["tag"].removeprefix("v")
        self.assertEqual(module.minimum_qca_version(), version)
        depends = module.qca_runtime_depends()
        self.assertEqual(depends,
                         f"libqca3-qt6-3 (>= {version}), "
                         f"libqca3-qt6-plugins (>= {version})")

    def test_changes_to_lock_change_both_runtime_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            lock_path = Path(directory) / "dependencies.lock.json"
            for tag in ("v3.0.1", "v3.2.14"):
                lock_path.write_text(json.dumps({"qca": {"tag": tag}}),
                                     encoding="utf-8")
                version = tag[1:]
                self.assertEqual(module.minimum_qca_version(lock_path), version)
                self.assertEqual(module.qca_runtime_depends(lock_path),
                                 f"libqca3-qt6-3 (>= {version}), "
                                 f"libqca3-qt6-plugins (>= {version})")

    def test_invalid_tag_fails_loudly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lock.json"
            for invalid in ("latest", "v3.0", "v3.0.0-rc1", ""):
                path.write_text(json.dumps({"qca": {"tag": invalid}}),
                                encoding="utf-8")
                with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                    module.minimum_qca_version(path)

    def test_controls_derive_minimum_from_substitution(self):
        for filename in CONTROLS:
            with self.subTest(filename=filename):
                content = (ROOT / filename).read_text(encoding="utf-8")
                stanza = content.split("Package: libanykeep3", 1)[1].split(
                    "Package: libanykeep-dev", 1)[0]
                self.assertIn("${qca:Depends}", stanza)
                self.assertNotIn("libqca3-qt6-3 (>=", stanza)
                self.assertNotIn("libqca3-qt6-plugins (>=", stanza)

    def test_gencontrol_generates_lock_driven_dependencies(self):
        rules = (ROOT / "debian/rules").read_text(encoding="utf-8")
        self.assertIn("override_dh_gencontrol:", rules)
        self.assertIn("qca-depends.py --depends", rules)
        self.assertIn("-Vqca:Depends=", rules)


if __name__ == "__main__":
    unittest.main()
