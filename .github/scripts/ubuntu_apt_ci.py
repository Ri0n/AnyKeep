#!/usr/bin/env python3
"""Bound Ubuntu CI APT network time and retry against the public Ubuntu archive.

Only the download phases are time-limited. dpkg installation has no
deadline so a slow maintainer script cannot corrupt package state.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

NETWORK_SECONDS = 120
PUBLIC_ARCHIVE = "http://archive.ubuntu.com/ubuntu"
AZURE_ARCHIVE = "http://azure.archive.ubuntu.com/ubuntu"
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


def normalize_local_debs(arguments: list[str]) -> list[str]:
    # APT rejects relative local .deb names in its --no-download install phase.
    return [
        str(Path(arg).resolve())
        if arg.endswith(".deb") and arg.startswith(("./", "../")) else arg
        for arg in arguments
    ]


def install_cached_apt(arguments: list[str]) -> None:
    # APT's --no-download mode incorrectly rejects locally provided .deb
    # archives, even when their path is absolute (observed on Ubuntu 24.04
    # and 26.04). Those archives and their dependencies were prefetched
    # above, so use normal APT installation for this case. Never put a
    # timeout around dpkg's configuration phase.
    local_debs = any(arg.endswith(".deb") and Path(arg).is_absolute()
                     for arg in arguments)
    command = [
        *root_command(), "apt-get",
        "-o", "Acquire::Retries=0",
        "-o", "Acquire::http::Timeout=20",
        "-o", "Acquire::https::Timeout=20",
        *([] if local_debs else ["--no-download"]),
        *arguments,
    ]
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
        original = mirrors.read_text(encoding="utf-8")
        # Azure-backed Ubuntu archive has been fast on hosted runners while
        # public archive IPs sometimes reject connections. Keep both mirrors.
        fallback_order = ([PUBLIC_ARCHIVE, AZURE_ARCHIVE]
                          if AZURE_ARCHIVE in original
                          else [AZURE_ARCHIVE, PUBLIC_ARCHIVE])
        new_content = "\n".join(fallback_order) + "\n"
        if original != new_content:
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

    # A mirror+file source retains its index identity when its backing
    # mirrorlist changes. Refreshing it is unnecessary and can itself fail on
    # a temporarily unavailable InRelease endpoint. Direct source rewrites
    # change the source identity and DO require an index refresh.
    changed_sources = any(Path(path).name != "apt-mirrors.txt" for path in switched)
    if arguments != ["update"] and changed_sources and bounded_apt(["update"]) != 0:
        raise RuntimeError("APT index refresh failed on fallback mirrors")
    if bounded_apt(arguments) != 0:
        raise RuntimeError("APT download failed on fallback mirrors")


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
    if operation == "install":
        arguments = normalize_local_debs(arguments)
    require_download(["--download-only", operation, *arguments])
    install_cached_apt([operation, *arguments])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"::error::{error}")
