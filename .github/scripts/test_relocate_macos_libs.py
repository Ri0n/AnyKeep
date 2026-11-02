"""Unit tests for the macOS non-Qt dependency relocation helper."""
import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("relocate-macos-libs.py")
SPEC = importlib.util.spec_from_file_location("relocate_macos_libs", SCRIPT)
RELOCATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELOCATE)


class RelocateMacosLibsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.bundle = Path(self.temp.name) / "AnyKeep.app"
        self.frameworks = self.bundle / "Contents" / "Frameworks"
        self.frameworks.mkdir(parents=True)

    def macho(self, relative):
        path = self.bundle / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(bytes.fromhex("cffaedfe") + b"test")
        return path

    def test_patch_rewrites_executable_and_nested_plugin_load_paths(self):
        executable = self.macho("Contents/MacOS/anykeep")
        provider = self.macho("Contents/PlugIns/qca3-qt6/crypto/libqca-ossl.dylib")
        library = self.macho("Contents/Frameworks/libiris-qt6.1.dylib")
        self.macho("Contents/Frameworks/libanykeep.3.dylib")
        self.macho("Contents/Frameworks/libqca3-qt6.3.dylib")
        references = {
            executable: {"@rpath/libanykeep.3.dylib", "@rpath/libqca3-qt6.3.dylib"},
            provider: {"@rpath/libqca3-qt6.3.dylib"},
            library: {"@rpath/libqca3-qt6.3.dylib"},
        }
        changes = []

        def fake_deps(path):
            return references.get(path, set())

        def fake_command(*args, **_kwargs):
            self.assertEqual(args[0:2], ("install_name_tool", "-change"))
            changes.append(args[2:])

        with patch.object(RELOCATE, "dependencies", side_effect=fake_deps), \
             patch.object(RELOCATE, "install_id", return_value=None), \
             patch.object(RELOCATE, "command", side_effect=fake_command):
            RELOCATE.relocate(self.bundle)

        self.assertIn(
            ("@rpath/libanykeep.3.dylib",
             "@loader_path/../Frameworks/libanykeep.3.dylib", str(executable)), changes
        )
        self.assertIn(
            ("@rpath/libqca3-qt6.3.dylib",
             "@loader_path/../../../Frameworks/libqca3-qt6.3.dylib", str(provider)), changes
        )
        self.assertIn(
            ("@rpath/libqca3-qt6.3.dylib",
             "@loader_path/libqca3-qt6.3.dylib", str(library)), changes
        )

    def test_verify_rejects_missing_or_unrelocated_library(self):
        executable = self.macho("Contents/MacOS/anykeep")
        lib = self.macho("Contents/Frameworks/libanykeep.3.dylib")
        with patch.object(RELOCATE, "dependencies", return_value={"@rpath/libanykeep.3.dylib"}), \
             patch.object(RELOCATE, "install_id", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "unrelocated"):
                RELOCATE.verify(self.bundle)
        self.assertTrue(lib.is_file())

    def test_verify_accepts_resolved_loader_relative_library(self):
        executable = self.macho("Contents/MacOS/anykeep")
        self.macho("Contents/Frameworks/libanykeep.3.dylib")

        def fake_deps(path):
            if path == executable:
                return {"@loader_path/../Frameworks/libanykeep.3.dylib"}
            return set()

        with patch.object(RELOCATE, "dependencies", side_effect=fake_deps), \
             patch.object(RELOCATE, "install_id", return_value=None):
            RELOCATE.verify(self.bundle)


if __name__ == "__main__":
    unittest.main()
