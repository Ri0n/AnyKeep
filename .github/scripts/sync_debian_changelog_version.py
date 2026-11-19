#!/usr/bin/env python3
"""Align CI Debian package versions with AnyKeep's Git-derived CMake version.

A static debian/changelog can lag behind nightly and stable release tags.
Modify only the runner checkout, never the tracked release history.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DESCRIBE_PATTERN = re.compile(
    r"v?([0-9]+)\.([0-9]+)\.([0-9]+)"
    r"(?:[_-](rc|alpha|beta)([0-9]+))?"
    r"-([0-9]+)-g[0-9a-f]+"
)
CHANGELOG_HEADER = re.compile(r"^(anykeep \()[^)]+(\) .*)$", re.MULTILINE)


def package_version(describe: str) -> str:
    match = DESCRIBE_PATTERN.fullmatch(describe.strip())
    if not match:
        raise ValueError(f"Unexpected application Git description: {describe!r}")
    major, minor, patch, prerelease, number, distance = match.groups()
    version = f"{int(major)}.{int(minor)}.{int(patch) + int(distance)}"
    # Debian sorts prereleases below their eventual stable counterpart.
    if prerelease:
        version += f"~{prerelease}{number}"
    return version


def update_changelog(content: str, version: str) -> str:
    updated, count = CHANGELOG_HEADER.subn(
        lambda match: match.group(1) + version + match.group(2),
        content, count=1,
    )
    if count != 1:
        raise ValueError("Expected an AnyKeep Debian changelog header")
    return updated


def main() -> None:
    # The Debian container can run under a different UID than the checkout
    # owner. Trust only this exact GitHub Actions workspace, not all paths.
    result = subprocess.run(
        [
            "git", "-C", str(ROOT), "-c", f"safe.directory={ROOT}",
            "describe", "--tags", "--long",
            "--match", "v[0-9]*", "--match", "[0-9]*",
        ],
        capture_output=True, text=True, check=False,
    )
    if result.returncode:
        raise RuntimeError(f"Could not describe checkout: {result.stderr.strip()}")
    version = package_version(result.stdout)
    changelog = ROOT / "debian/changelog"
    original = changelog.read_text(encoding="utf-8")
    updated = update_changelog(original, version)
    if updated != original:
        changelog.write_text(updated, encoding="utf-8")
    print(f"Debian packaging version: {version} (from {result.stdout.strip()})", flush=True)


if __name__ == "__main__":
    main()
