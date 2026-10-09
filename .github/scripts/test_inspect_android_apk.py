#!/usr/bin/env python3
"""Unit tests for the Android APK library inventory guard."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile


module_path = Path(__file__).with_name("inspect-android-apk.py")
spec = importlib.util.spec_from_file_location("inspect_android_apk", module_path)
inspector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspector)


class ApkInventoryTests(unittest.TestCase):
    ABI = "arm64-v8a"

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.apk = Path(directory.name) / "test.apk"
        self.app_name = f"lib/{self.ABI}/libanykeep_{self.ABI}.so"
        self.app_bytes = b"mock-elf"
        self.plugins = [
            f"lib/{self.ABI}/libplugins_platforms_qtforandroid_{self.ABI}.so",
            f"lib/{self.ABI}/libplugins_tls_qopensslbackend_{self.ABI}.so",
            f"lib/{self.ABI}/libplugins_multimedia_androidmediaplugin_{self.ABI}.so",
            f"lib/{self.ABI}/libqml_QtQuick_Controls_Material_qtquickcontrols2materialstyleplugin_{self.ABI}.so",
            f"lib/{self.ABI}/libqml_QtQuick_Controls_Basic_qtquickcontrols2basicstyleplugin_{self.ABI}.so",
        ]

    def make_apk(self, *, excluded=(), included=()):
        with zipfile.ZipFile(self.apk, "w") as archive:
            archive.writestr(self.app_name, self.app_bytes)
            for path in self.plugins:
                if path not in excluded:
                    archive.writestr(path, b"plugin")
            for path in included:
                archive.writestr(path, b"extra")

    def verify(self, linked_size=None):
        return inspector.inspect_apk(
            self.apk,
            self.ABI,
            len(self.app_bytes) if linked_size is None else linked_size,
        )

    def test_minimal_runtime_plugins_pass(self):
        self.make_apk()
        report = self.verify()
        self.assertIn("QML tooling: **0**", report)
        self.assertIn("Android APK inventory: arm64-v8a", report)

    def test_qml_tooling_rejected(self):
        self.make_apk(included=[
            f"lib/{self.ABI}/libplugins_qmltooling_qmldbg_debugger_{self.ABI}.so"
        ])
        with self.assertRaisesRegex(ValueError, "QML debug/profiling"):
            self.verify()

    def test_missing_runtime_plugin_rejected(self):
        self.make_apk(excluded=(self.plugins[2],))
        with self.assertRaisesRegex(ValueError, "Essential Qt plugins missing"):
            self.verify()

    def test_unused_controls_style_rejected(self):
        self.make_apk(included=[
            f"lib/{self.ABI}/libqml_QtQuick_Controls_Fusion_qtquickcontrols2fusionstyleplugin_{self.ABI}.so"
        ])
        with self.assertRaisesRegex(ValueError, "Unused Qt Quick Controls"):
            self.verify()

    def test_packaged_elf_mismatch_rejected(self):
        self.make_apk()
        with self.assertRaisesRegex(ValueError, "differs from the verified linked ELF"):
            self.verify(linked_size=len(self.app_bytes) + 1)

    def test_wrong_abi_rejected(self):
        self.make_apk(included=["lib/x86_64/libunexpected.so"])
        with self.assertRaisesRegex(ValueError, "other ABIs"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
