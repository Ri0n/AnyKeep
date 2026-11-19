#!/usr/bin/env python3
"""Bound Ubuntu CI APT network time and retry against the public Ubuntu archive.

Only the download phases are time-limited. dpkg installation runs offline
without a deadline so a slow maintainer script cannot corrupt package state.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

NETWORK_SECONDS = 120
PUBLIC_ARCHIVE = "http://archive.ubuntu.com/ubuntu"
REGIONAL_ARCHIVE = re.compile(
    r"https?://(?:[a-z0-9-]+\.)*archive\.ubuntu\.com/ubuntu(?=/|\s|$)", re.I
)


def root_command() -> list[str]:
    return [] if os.geteuid() == 0 else ["sudo", "-n"]


def bounded_apt(arguments: list[str]) -> int:
    command = [
        *root_command(),
        "timeout", "--signal=TERM", "--kill-after=10s",
        f"{NETWORK_SECONDS}s",
        "apt-get",
        "-o", "Acquire::Retries=0",
        "-o", "Acquire::http::Timeout=20",
        "-o", "Acquire::https::Timeout=20",
        "-o", "APT::Update::Error-Mode=any",
        *arguments,
    ]
    print("+", " ".join(command), flush=True)
    return subprocess.run(command, check=False).returncode


def offline_apt(arguments: list[str]) -> None:
    command = [*root_command(), "apt-get", "--no-download", *arguments]
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def rewrite_archive_urls(content: str) -> str:
    # Leave security.ubuntu.com and third-party repositories untouched.
    return REGIONAL_ARCHIVE.sub(PUBLIC_ARCHIVE, content)


def write_system_file(path: Path, content: str) -> None:
    if os.geteuid() == 0:
        path.write_text(content, encoding="utf-8")
    else:
        subprocess.run(
            ["sudo", "-n", "tee", str(path)],
            input=content, text=True, stdout=subprocess.DEVNULL, check=True,
        )


def switch_to_archive(apt_root: Path = Path("/etc/apt")) -> list[str]:
    changed: list[str] = []
    mirrors = apt_root / "apt-mirrors.txt"
    if mirrors.is_file():
        new_content = PUBLIC_ARCHIVE + "\n"
        if mirrors.read_text(encoding="utf-8") != new_content:
            write_system_file(mirrors, new_content)
            changed.append(str(mirrors))

    # Handle both legacy sources.list and current Deb822 ubuntu.sources.
    sources = [apt_root / "sources.list"]
    for suffix in ("*.list", "*.sources"):
        sources.extend(sorted((apt_root / "sources.list.d").glob(suffix)))
    for path in sources:
        if not path.is_file():
            continue
        old = path.read_text(encoding="utf-8")
        new = rewrite_archive_urls(old)
        if new != old:
            write_system_file(path, new)
            changed.append(str(path))
    return changed


def require_download(arguments: list[str]) -> None:
    if bounded_apt(arguments) == 0:
        return

    print("::warning::Ubuntu APT download failed or exceeded 120s; switching archive mirror", flush=True)
    switched = switch_to_archive()
    if not switched:
        raise RuntimeError("APT download failed and no regional Ubuntu archive was available for fallback")
    print("APT fallback changed:", ", ".join(switched), flush=True)

    # After changing APT sources, refresh indexes before downloading packages.
    if arguments != ["update"] and bounded_apt(["update"]) != 0:
        raise RuntimeError("APT index refresh failed on the public Ubuntu archive")
    if bounded_apt(arguments) != 0:
        raise RuntimeError("APT download failed on the public Ubuntu archive")


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in ("update", "install", "build-dep"):
        raise SystemExit("Usage: ubuntu_apt_ci.py update|install|build-dep [apt arguments...]")
    operation, *arguments = sys.argv[1:]
    if operation == "update":
        if arguments:
            raise SystemExit("update accepts no arguments")
        require_download(["update"])
        return
    if not arguments:
        raise SystemExit(f"{operation} requires package arguments")
    require_download(["--download-only", operation, *arguments])
    offline_apt([operation, *arguments])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"::error::{error}")
