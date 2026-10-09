#!/usr/bin/env python3
"""Guard XMPP QML's light/dark secondary-text contrast.

The Android QPalette supplies distinct roles for borders (Mid) and secondary
text (PlaceholderText). This test keeps those roles distinct and checks the
worst-case dialog surface in both supported palettes.
"""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / "plugins/xmpppubsub"
PALETTE_IMPL = ROOT / "src/mobile/mobileapplication.cpp"
QML_FILES = (
    "XmppKeyResolutionHost.qml",
    "XmppKeySyncTrustHost.qml",
    "XmppSettings.qml",
)


def luminance(hex_rgb):
    channels = [int(hex_rgb[i : i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return sum(a * b for a, b in zip(linear, (0.2126, 0.7152, 0.0722)))


def contrast(foreground, background):
    lighter, darker = sorted((luminance(foreground), luminance(background)), reverse=True)
    return (lighter + 0.05) / (darker + 0.05)


def palette_color(role, dark):
    source = PALETTE_IMPL.read_text(encoding="utf-8")
    pattern = rf"const QColor\s+{role}\s*=\s*dark\s*\?\s*QColor\(QStringLiteral\(\"(#[0-9a-fA-F]{{6}})\"\)\)\s*:\s*QColor\(QStringLiteral\(\"(#[0-9a-fA-F]{{6}})\"\)\)"
    match = re.search(pattern, source)
    if not match:
        raise AssertionError(f"Cannot find {role} theme colors in MobileApplication::applyColorScheme")
    return match.group(1 if dark else 2)


class XmpThemeContrastTests(unittest.TestCase):
    def test_secondary_colors_retain_legible_contrast_in_both_themes(self):
        for dark in (False, True):
            with self.subTest(dark=dark):
                muted = palette_color("muted", dark)
                window = palette_color("window", dark)
                base = palette_color("base", dark)
                self.assertGreaterEqual(contrast(muted, window), 4.5)
                self.assertGreaterEqual(contrast(muted, base), 4.5)
                if dark:
                    # The Material Dialog shown on Android is lighter than
                    # the generic dark Window surface (approximately #404040).
                    self.assertGreaterEqual(contrast(muted, "#404040"), 4.5)

    def test_recovery_uses_mid_only_for_divider_not_text(self):
        qml = (PLUGIN / "XmppKeyResolutionHost.qml").read_text(encoding="utf-8")
        self.assertEqual(qml.count("palette.mid"), 1)
        self.assertRegex(qml, r"Rectangle\s*\{\s*Layout.fillWidth: true\s*implicitHeight: 1\s*color: palette.mid")
        self.assertIn("text: root.pageSubtitle()", qml)
        self.assertIn("color: palette.placeholderText", qml)

    def test_other_xmpp_secondary_labels_not_border_colored(self):
        for filename in QML_FILES:
            with self.subTest(filename=filename):
                qml = (PLUGIN / filename).read_text(encoding="utf-8")
                self.assertNotRegex(qml, r"color:\s*palette.mid\b")
                if filename != "XmppKeyResolutionHost.qml":
                    self.assertIn("color: palette.placeholderText", qml)

    def test_unavailable_key_explanation_not_dimmed_as_a_whole(self):
        qml = (PLUGIN / "XmppKeyResolutionHost.qml").read_text(encoding="utf-8")
        self.assertNotIn("opacity: keyDelegate.available", qml)
        self.assertIn("enabled: keyDelegate.available && !root.controller.busy", qml)
        self.assertIn("color: keyDelegate.available ? palette.text : palette.placeholderText", qml)


if __name__ == "__main__":
    unittest.main()
