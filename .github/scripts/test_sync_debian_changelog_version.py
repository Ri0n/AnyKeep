"""Regression coverage for nightly and stable Debian package versions."""
import unittest

from sync_debian_changelog_version import package_version, update_changelog


class DebianVersionTests(unittest.TestCase):
    def test_nightly_commit_distance(self):
        self.assertEqual(package_version("4.0.5-23-gc6712133"), "4.0.28")

    def test_exact_release_tag(self):
        self.assertEqual(package_version("4.0.29-0-gabcdef12"), "4.0.29")
        self.assertEqual(package_version("v4.1.0-0-gabcdef12"), "4.1.0")

    def test_release_candidate(self):
        self.assertEqual(package_version("4.0.29_rc2-0-gabcdef12"), "4.0.29~rc2")

    def test_invalid_git_description_is_not_silently_published(self):
        for value in ("deps-qtkeychain-0.14.3", "nightly", "4.0.29", ""):
            with self.subTest(value=value), self.assertRaises(ValueError):
                package_version(value)

    def test_rewrite_only_changelog_header(self):
        original = (
            "anykeep (4.0.5) unstable; urgency=high\n\n"
            "  * Existing release notes.\n\n"
            " -- Maintainer <example@example.com>  Mon, 09 Nov 2026 19:20:00 +0300\n"
        )
        updated = update_changelog(original, "4.0.29")
        self.assertEqual(
            updated, original.replace("anykeep (4.0.5)", "anykeep (4.0.29)", 1)
        )

    def test_reject_missing_changelog_header(self):
        with self.assertRaises(ValueError):
            update_changelog("not a changelog\n", "4.0.29")


if __name__ == "__main__":
    unittest.main()
