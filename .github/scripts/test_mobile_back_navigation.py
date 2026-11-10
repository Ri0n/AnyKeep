"""Protect Android Back navigation from regressing to activity backgrounding."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "src/mobile/Main.qml"
EDITOR = ROOT / "src/mobile/NoteEditorPage.qml"


class AndroidBackNavigationTest(unittest.TestCase):
    def test_window_shortcut_handles_back_while_a_page_is_open(self):
        qml = MAIN.read_text(encoding="utf-8")
        self.assertRegex(
            qml,
            r'Shortcut\s*\{\s*sequences:\s*\["Back"\]\s*'
            r'enabled:\s*Qt\.platform\.os\s*===\s*"android"\s*&&\s*'
            r'navigation\.depth\s*>\s*1\s*'
            r'onActivated:\s*window\.navigateBack\(\)',
        )
        self.assertNotIn("StandardKey.Back", qml)

    def test_android_back_uses_same_safe_close_path_as_window_close(self):
        qml = MAIN.read_text(encoding="utf-8")
        self.assertRegex(
            qml,
            r'function navigateBack\(\)\s*\{\s*'
            r'if \(navigation\.depth <= 1\)\s*return\s*'
            r'const page = navigation\.currentItem\s*'
            r'if \(page && typeof page\.closeEditor === "function"\)\s*'
            r'page\.closeEditor\(\)\s*else\s*navigation\.pop\(\)',
        )
        self.assertRegex(
            qml,
            r'onClosing:\s*function\(close\)\s*\{\s*'
            r'if \(navigation\.depth <= 1\)\s*return\s*'
            r'close\.accepted = false\s*window\.navigateBack\(\)',
        )

        editor = EDITOR.read_text(encoding="utf-8")
        # Do not skip draft-aware close/save handling for a new note.
        self.assertRegex(
            editor,
            r'function closeEditor\(\)\s*\{\s*'
            r'if \(editorPane\.closeEditor\(\)\)\s*backRequested\(\)',
        )
        self.assertIn('closeHandler: function() { return mobileApp.closeCurrentNote() }', editor)


if __name__ == "__main__":
    unittest.main()
