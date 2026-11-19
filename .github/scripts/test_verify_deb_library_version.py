"""Regression tests for the actual .deb's versioned shared-library validation."""

import unittest

from verify_deb_library_version import verify


def listing(*paths):
    return "\n".join(
        f"-rw-r--r-- root/root 100 2026-11-19 15:00 ./usr/lib/x86_64-linux-gnu/{path}"
        for path in paths
    )


class LibraryVersionTests(unittest.TestCase):
    def test_exact_release(self):
        self.assertEqual(verify("4.0.30", listing("libanykeep.so.4.0.30")), "4.0.30")

    def test_debian_revision_and_prerelease(self):
        self.assertEqual(verify("4.0.30-1", listing("libanykeep.so.4.0.30")), "4.0.30")
        self.assertEqual(verify("4.0.30~rc1", listing("libanykeep.so.4.0.30")), "4.0.30")

    def test_reject_previous_library_from_current_release(self):
        with self.assertRaisesRegex(ValueError, "4.0.5"):
            verify("4.0.30", listing("libanykeep.so.4.0.5"))

    def test_reject_symlink_without_library(self):
        content = (
            "lrwxrwxrwx root/root 0 2026-11-19 15:00 "
            "./usr/lib/x86_64-linux-gnu/libanykeep.so.3 -> libanykeep.so.4.0.30"
        )
        with self.assertRaisesRegex(ValueError, "found versioned libraries"):
            verify("4.0.30", content)

    def test_reject_mixed_versions(self):
        with self.assertRaisesRegex(ValueError, "found versioned libraries"):
            verify("4.0.30", listing("libanykeep.so.4.0.30", "libanykeep.so.4.0.5"))

    def test_reject_bad_debian_version(self):
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            verify("nightly", listing("libanykeep.so.4.0.30"))


if __name__ == "__main__":
    unittest.main()
