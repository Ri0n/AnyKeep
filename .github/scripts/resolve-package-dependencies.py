#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

REQUIRED_ASSETS = {
    "iris": {
        "iris-deb-ubuntu-24.04.zip",
        "iris-deb-ubuntu-26.04.zip",
        "iris-deb-debian-13.zip",
        "iris-qt6-windows-x64.zip",
        "iris-qt6-macos-arm64.zip",
        "iris-qt6-macos-x86_64.zip",
        "iris-qt6-android-arm64-v8a.zip",
        "iris-qt6-android-x86_64.zip",
    },
    "qca": {
        "qca3-deb-ubuntu-24.04.zip",
        "qca3-deb-ubuntu-26.04.zip",
        "qca3-deb-debian-13.zip",
        "qca3-qt6-windows-x64.zip",
        "qca3-qt6-macos-arm64.zip",
        "qca3-qt6-macos-x86_64.zip",
        "qca3-qt6-android-arm64-v8a.zip",
        "qca3-qt6-android-x86_64.zip",
    },
}

REPOSITORIES = {
    "iris": "psi-im/iris",
    "qca": "psi-im/qca",
}

SEMVER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")
QTKEYCHAIN_TAG_RE = re.compile(
    r"^deps-qtkeychain-(\d+)\.(\d+)\.(\d+)-r(\d+)-qt6\.11$"
)


def parse_version(tag: str) -> tuple[int, int, int]:
    match = SEMVER_RE.fullmatch(tag)
    if not match:
        raise ValueError(f"Unsupported semantic version tag: {tag}")
    return tuple(int(part) for part in match.groups())


def gh_json(endpoint: str):
    result = subprocess.run(
        ["gh", "api", endpoint],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return json.loads(result.stdout)


def latest_compatible_release(name: str, locked: dict) -> str:
    locked_tag = locked["tag"]
    locked_version = parse_version(locked_tag)
    policy = locked.get("nightly_compatibility", "locked")

    if policy == "locked":
        return locked_tag
    if policy != "same-minor":
        raise RuntimeError(f"Unsupported nightly compatibility policy for {name}: {policy}")

    releases = gh_json(f"/repos/{REPOSITORIES[name]}/releases?per_page=100")
    candidates: list[tuple[tuple[int, int, int], str]] = []

    for release in releases:
        if release.get("draft") or release.get("prerelease"):
            continue
        tag = release.get("tag_name", "")
        try:
            version = parse_version(tag)
        except ValueError:
            continue
        if version[:2] != locked_version[:2] or version < locked_version:
            continue

        assets = {asset.get("name", "") for asset in release.get("assets", [])}
        if not REQUIRED_ASSETS[name].issubset(assets):
            continue

        candidates.append((version, tag))

    if not candidates:
        return locked_tag
    return max(candidates)[1]


def latest_compatible_qtkeychain(locked: dict) -> tuple[str, str, str]:
    locked_version = parse_version(locked["version"])
    locked_revision_match = re.fullmatch(r"r(\d+)", locked["revision"])
    if not locked_revision_match:
        raise ValueError(f"Unsupported QtKeychain revision: {locked['revision']}")
    locked_revision = int(locked_revision_match.group(1))

    policy = locked.get("nightly_compatibility", "locked")
    if policy == "locked":
        return locked["tag"], locked["version"], locked["revision"]
    if policy != "same-minor":
        raise RuntimeError(f"Unsupported QtKeychain nightly compatibility policy: {policy}")

    releases = gh_json("/repos/Ri0n/AnyKeep/releases?per_page=100")
    candidates: list[tuple[tuple[int, int, int], int, str, str, str]] = []

    for release in releases:
        if release.get("draft"):
            continue
        tag = release.get("tag_name", "")
        match = QTKEYCHAIN_TAG_RE.fullmatch(tag)
        if not match:
            continue

        version = tuple(int(part) for part in match.groups()[:3])
        revision = int(match.group(4))
        if version[:2] != locked_version[:2]:
            continue
        if (version, revision) < (locked_version, locked_revision):
            continue

        version_text = ".".join(str(part) for part in version)
        revision_text = f"r{revision}"
        prefix = f"qtkeychain-{version_text}-{revision_text}-qt6.11"
        required_assets = {
            f"{prefix}-windows-x64.tar.gz",
            f"{prefix}-macos-arm64.tar.gz",
            f"{prefix}-macos-x86_64.tar.gz",
            f"{prefix}-android-arm64-v8a.tar.gz",
            f"{prefix}-android-x86_64.tar.gz",
        }
        assets = {asset.get("name", "") for asset in release.get("assets", [])}
        if not required_assets.issubset(assets):
            continue

        candidates.append((version, revision, tag, version_text, revision_text))

    if not candidates:
        return locked["tag"], locked["version"], locked["revision"]

    _, _, tag, version_text, revision_text = max(candidates)
    return tag, version_text, revision_text


def write_output(name: str, value: str) -> None:
    if "\n" in value or "\r" in value:
        raise RuntimeError(f"Invalid multiline output for {name}")
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as handle:
            handle.write(f"{name}={value}\n")
    else:
        print(f"{name}={value}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("stable", "nightly"), required=True)
    parser.add_argument("--lock", default="dependencies.lock.json")
    args = parser.parse_args()

    lock = json.loads(Path(args.lock).read_text(encoding="utf-8"))

    iris_tag = lock["iris"]["tag"]
    qca_tag = lock["qca"]["tag"]
    if args.mode == "nightly":
        iris_tag = latest_compatible_release("iris", lock["iris"])
        qca_tag = latest_compatible_release("qca", lock["qca"])

    qtkeychain = lock["qtkeychain"]
    qtkeychain_tag = qtkeychain["tag"]
    qtkeychain_version = qtkeychain["version"]
    qtkeychain_revision = qtkeychain["revision"]
    if args.mode == "nightly":
        qtkeychain_tag, qtkeychain_version, qtkeychain_revision = latest_compatible_qtkeychain(qtkeychain)

    summary = (
        f"iris={iris_tag};"
        f"qca={qca_tag};"
        f"qtkeychain={qtkeychain_tag}"
    )
    fingerprint = hashlib.sha256(summary.encode("utf-8")).hexdigest()

    write_output("iris_tag", iris_tag)
    write_output("qca_tag", qca_tag)
    write_output("qtkeychain_tag", qtkeychain_tag)
    write_output("qtkeychain_version", qtkeychain_version)
    write_output("qtkeychain_revision", qtkeychain_revision)
    write_output("dependency_summary", summary)
    write_output("dependency_fingerprint", fingerprint)


if __name__ == "__main__":
    main()
