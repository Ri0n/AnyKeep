#!/usr/bin/env python3
"""Generate Debian runtime requirements from dependencies.lock.json.

Each provider's minimum_version is independent from its selected stable tag
and the newer compatible tags permitted for nightly builds.
"""

import argparse
import json
from pathlib import Path
import re


DEFAULT_LOCK = Path(__file__).resolve().parents[2] / "dependencies.lock.json"
VERSION_PATTERN = re.compile(r"v?(\d+)\.(\d+)\.(\d+)\Z")
RUNTIME_PACKAGES = {
    "qca": ("libqca3-qt6-3", "libqca3-qt6-plugins"),
    "iris": ("libiris-qt6-1",),
}


def version_tuple(value: str) -> tuple[int, int, int]:
    if not isinstance(value, str):
        raise ValueError(f"Expected a semantic version string, got {value!r}")
    match = VERSION_PATTERN.fullmatch(value)
    if not match:
        raise ValueError(f"Invalid semantic version: {value!r}")
    return tuple(map(int, match.groups()))


def locked_minimums(lock_path: Path = DEFAULT_LOCK) -> dict[str, str]:
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    result = {}
    for name in RUNTIME_PACKAGES:
        dependency = lock[name]
        minimum = dependency["minimum_version"]
        pinned = dependency["tag"]
        floor_version = version_tuple(minimum)
        pinned_version = version_tuple(pinned)
        if pinned_version < floor_version:
            raise ValueError(
                f"{name}: stable tag {pinned} is older than minimum_version {minimum}"
            )
        result[name] = ".".join(map(str, floor_version))
    return result


def runtime_depends(lock_path: Path = DEFAULT_LOCK) -> str:
    minima = locked_minimums(lock_path)
    return ", ".join(
        f"{package} (>= {minima[name]})"
        for name, packages in RUNTIME_PACKAGES.items()
        for package in packages
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--depends", action="store_true",
                       help="Emit comma-separated Debian runtime dependencies")
    group.add_argument("--minimum", choices=RUNTIME_PACKAGES,
                       help="Emit the requested minimum version")
    args = parser.parse_args()
    print(runtime_depends(args.lock) if args.depends
          else locked_minimums(args.lock)[args.minimum])


if __name__ == "__main__":
    main()
