"""Tests for Ubuntu CI APT source rewriting and bounded fallback."""
import tempfile
from pathlib import Path
from unittest import TestCase
from unittest.mock import call, patch

import ubuntu_apt_ci as apt


class UbuntuAptCITest(TestCase):
    def test_rewrites_regional_url_but_not_security_or_third_party(self):
        content = (
            "URIs: http://us-east-2.ec2.archive.ubuntu.com/ubuntu/\n"
            "URIs: https://azure.archive.ubuntu.com/ubuntu\n"
            "URIs: http://security.ubuntu.com/ubuntu\n"
            "deb https://packages.microsoft.com/ubuntu/24.04/prod noble main\n"
        )
        changed = apt.rewrite_archive_urls(content)
        self.assertIn("URIs: http://archive.ubuntu.com/ubuntu/\n", changed)
        self.assertIn("URIs: http://archive.ubuntu.com/ubuntu\n", changed)
        self.assertIn("URIs: http://security.ubuntu.com/ubuntu", changed)
        self.assertIn("deb https://packages.microsoft.com/ubuntu/24.04/prod", changed)

    def test_sources_and_mirrorlist_without_touching_other_repositories(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "sources.list.d").mkdir()
            (root / "apt-mirrors.txt").write_text(
                "http://us-east-2.ec2.archive.ubuntu.com/ubuntu\n"
                "http://archive.ubuntu.com/ubuntu\n"
            )
            ubuntu = root / "sources.list.d/ubuntu.sources"
            ubuntu.write_text("URIs: mirror+file:/etc/apt/apt-mirrors.txt\n"
                              "URIs: http://azure.archive.ubuntu.com/ubuntu\n")
            third_party = root / "sources.list.d/vendor.list"
            third_party.write_text("deb https://packages.microsoft.com/ubuntu/24.04/prod noble main\n")
            with patch.object(apt, "write_system_file", side_effect=lambda p, s: p.write_text(s)):
                paths = apt.switch_to_archive(root)
            self.assertEqual(len(paths), 2)
            self.assertEqual((root / "apt-mirrors.txt").read_text(), apt.PUBLIC_ARCHIVE + "\n")
            self.assertIn("URIs: http://archive.ubuntu.com/ubuntu", ubuntu.read_text())
            self.assertIn("mirror+file:", ubuntu.read_text())
            self.assertIn("packages.microsoft.com", third_party.read_text())

    def test_download_times_out_then_refreshes_and_retries(self):
        with patch.object(apt, "bounded_apt", side_effect=[124, 0, 0]) as bounded:
            with patch.object(apt, "switch_to_archive", return_value=["/etc/apt/apt-mirrors.txt"]):
                with patch.object(apt, "offline_apt") as offline:
                    apt.require_download(["--download-only", "install", "-y", "ccache"])
                    apt.offline_apt(["install", "-y", "ccache"])
        self.assertEqual(bounded.call_args_list, [
            call(["--download-only", "install", "-y", "ccache"]),
            call(["update"]),
            call(["--download-only", "install", "-y", "ccache"]),
        ])
        offline.assert_called_once_with(["install", "-y", "ccache"])

    def test_failed_fallback_does_not_run_offline_install(self):
        with patch.object(apt, "bounded_apt", side_effect=[124, 100]):
            with patch.object(apt, "switch_to_archive", return_value=["changed"]):
                with self.assertRaisesRegex(RuntimeError, "index refresh failed"):
                    apt.require_download(["--download-only", "install", "qt6-base-dev"])
