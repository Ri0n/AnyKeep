"""Validate visible Android settings controls and packaged QML translations."""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
MOBILE = ROOT / "src/mobile"


class AndroidLocalizationTests(unittest.TestCase):
    def test_settings_icon_uses_live_palette_instead_of_white_on_light_theme(self):
        qml = (MOBILE / "RootPage.qml").read_text(encoding="utf-8")
        self.assertIn('objectName: "androidSettingsButton"', qml)
        self.assertIn('fallbackTintMode: String(settingsButton.palette.buttonText)', qml)
        self.assertIn('themeName: "__bundled__"', qml)
        self.assertNotIn('fallbackTintMode: "light"', qml)
        self.assertIn('onClicked: root.openSettings()', qml)

    def test_mobile_translation_catalogs_are_embedded_and_loaded_before_qml(self):
        cmake = (MOBILE / "CMakeLists.txt").read_text(encoding="utf-8")
        main = (MOBILE / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("qt_add_translations(", cmake)
        self.assertIn('RESOURCE_PREFIX "/i18n"', cmake)
        self.assertIn('translations/anykeep_*.ts', cmake)
        self.assertIn('translations/anykeep_mobile_*.ts', cmake)
        self.assertIn('QStringLiteral(":/i18n")', main)
        self.assertIn('QStringLiteral("anykeep_mobile")', main)
        self.assertIn('QStringLiteral("anykeep")', main)
        self.assertLess(main.index('application.installTranslator(&mobileTranslator)'),
                        main.index('engine.load('))

    def test_russian_mobile_strings_exist_in_correct_qml_context(self):
        ts = MOBILE / "translations/anykeep_mobile_ru.ts"
        doc = ET.parse(ts)
        contexts = {
            ctx.findtext("name"): {
                msg.findtext("source"): msg.findtext("translation")
                for msg in ctx.findall("message")
                if msg.find("translation") is not None
                and msg.find("translation").get("type") != "unfinished"
            }
            for ctx in doc.getroot().findall("context")
        }
        self.assertEqual(contexts["RootPage"]["Notes"], "Заметки")
        self.assertEqual(contexts["RootPage"]["Settings"], "Настройки")
        self.assertEqual(contexts["NotesPage"]["Add note"], "Добавить заметку")
        self.assertEqual(contexts["SettingsHubPage"]["General"], "Общие")
        self.assertEqual(contexts["MobileNoteTopBar"]["Back"], "Назад")

    def test_every_mobile_translation_catalog_is_valid_xml(self):
        for ts in (MOBILE / "translations").glob("anykeep_mobile_*.ts"):
            with self.subTest(filename=ts.name):
                self.assertEqual(ET.parse(ts).getroot().tag, "TS")


if __name__ == "__main__":
    unittest.main()
