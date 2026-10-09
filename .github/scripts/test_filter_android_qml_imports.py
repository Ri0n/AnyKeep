#!/usr/bin/env python3
"""Unit tests for the Android QML import-scanner adapter."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


module_path = Path(__file__).with_name("filter-android-qml-imports.py")
spec = importlib.util.spec_from_file_location("filter_android_qml_imports", module_path)
filter_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(filter_module)


class AndroidQmlFilterTests(unittest.TestCase):
    def test_only_non_android_control_styles_removed(self):
        entries = [
            {"name": name, "type": "module", "path": "/qt/qml/" + name}
            for name in (
                "QtQuick", "QtQuick.Controls",
                "QtQuick.Controls.Material",
                "QtQuick.Controls.Basic",
                "QtQuick.Controls.Material.impl",
                "QtQuick.Controls.Fusion", "QtQuick.Controls.Fusion.impl",
                "QtQuick.Controls.Imagine", "QtQuick.Controls.Imagine.impl",
                "QtQuick.Controls.Universal", "QtQuick.Controls.Universal.impl",
                "QtQuick.Controls.FluentWinUI3", "QtQuick.Controls.FluentWinUI3.impl",
                "QtMultimedia",
            )
        ]
        kept, removed = filter_module.filter_imports(entries)
        self.assertEqual(len(removed), 8)
        self.assertEqual(len(kept), len(entries) - 8)
        self.assertIn("QtQuick.Controls.Material", [entry["name"] for entry in kept])
        self.assertIn("QtQuick.Controls.Basic", [entry["name"] for entry in kept])
        self.assertIn("QtMultimedia", [entry["name"] for entry in kept])

    def test_nonmodule_entries_not_filtered(self):
        value = {"type": "javascript", "name": "QtQuick.Controls.Fusion"}
        kept, removed = filter_module.filter_imports([value])
        self.assertEqual(kept, [value])
        self.assertEqual(removed, [])

    def test_invalid_scanner_response_fails(self):
        with self.assertRaisesRegex(ValueError, "JSON array"):
            filter_module.filter_imports({"name": "QtQuick.Controls"})
        with self.assertRaisesRegex(ValueError, "entry must be an object"):
            filter_module.filter_imports(["invalid"])

    def test_scanner_wrapper_passes_args_and_filters(self):
        with tempfile.TemporaryDirectory() as temp:
            scanner = Path(temp) / "qmlimportscanner"
            scanner.write_text(
                "#!/usr/bin/env python3\n"
                "import json, sys\n"
                "assert sys.argv[1:] == ['-rootPath', '/somewhere']\n"
                "print(json.dumps([{'name':'QtQuick.Controls.Fusion','type':'module'}, "
                "{'name':'QtQuick.Controls.Material','type':'module'}]))\n",
                encoding="utf-8",
            )
            scanner.chmod(0o755)
            with mock.patch.dict(os.environ, {"ANYKEEP_QMLIMPORTSCANNER_REAL": str(scanner)}):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    code = filter_module.scan(["-rootPath", "/somewhere"])
            self.assertEqual(code, 0)
            kept = json.loads(output.getvalue())
            self.assertEqual([entry["name"] for entry in kept], ["QtQuick.Controls.Material"])

    def test_configure_without_optional_qt_host_dir(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "Qt/6.11.3"
            android = root / "android_arm64_v8a"
            android.mkdir(parents=True)
            host = root / "gcc_64/libexec"
            host.mkdir(parents=True)
            scanner = host / "qmlimportscanner"
            scanner.write_text("#!/bin/sh\\nexit 0\\n", encoding="utf-8")
            scanner.chmod(0o755)
            settings = android / "deployment.json"
            settings.write_text(json.dumps({"qt": str(android), "qtLibExecsDirectory": {"arm64-v8a": str(android / "libexec")}}), encoding="utf-8")
            with mock.patch.dict(os.environ, {"QT_ROOT_DIR": str(android)}):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    result = filter_module.configure(settings)
            self.assertEqual(result, 0)
            self.assertEqual(output.getvalue().strip(), str(scanner.resolve()))

    def test_configure_preserves_other_deployment_settings(self):
        with tempfile.TemporaryDirectory() as temp:
            host = Path(temp)
            scanner = host / "libexec/qmlimportscanner"
            scanner.parent.mkdir(parents=True)
            scanner.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            scanner.chmod(0o755)
            settings = host / "android-settings.json"
            settings.write_text(
                json.dumps({"qtHostDir": str(host), "qt": "/some/qt/path", "android-min-sdk-version": 28}),
                encoding="utf-8",
            )
            with contextlib.redirect_stdout(io.StringIO()) as output:
                code = filter_module.configure(settings)
            self.assertEqual(code, 0)
            self.assertEqual(output.getvalue().strip(), str(scanner.resolve()))
            updated = json.loads(settings.read_text(encoding="utf-8"))
            self.assertEqual(updated["qt"], "/some/qt/path")
            self.assertEqual(updated["android-min-sdk-version"], 28)
            self.assertEqual(updated["qml-importscanner-binary"], str(module_path.resolve()))


if __name__ == "__main__":
    unittest.main()
