#!/usr/bin/env python3
"""Derive Debian QCA runtime dependencies from the project's dependency lock.

The QCA tag in dependencies.lock.json is both the stable build baseline and
AnyKeep's minimum supported QCA runtime version. Nightly builds may use a
newer compatible QCA, but Debian packages must not require its patch version.
"""

import argparse
import json
from pathlib import Path
import re


DEFAULT_LOCK = Path(__file__).resolve().parents[2] / "dependencies.lock.json"


def minimum_qca_version(lock_path: Path = DEFAULT_LOCK) -> str:
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    tag = lock["qca"]["tag"]
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", tag)
    if not match:
        raise ValueError(f"Invalid locked QCA release tag: {tag!r}")
    return ".".join(str(int(part)) for part in match.groups())


def qca_runtime_depends(lock_path: Path = DEFAULT_LOCK) -> str:
    version = minimum_qca_version(lock_path)
    return (
        f"libqca3-qt6-3 (>= {version}), "
        f"libqca3-qt6-plugins (>= {version})"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, default=DEFAULT_LOCK)
    parser.add_argument("--version", action="store_true",
                        help="Print the locked minimum version")
    parser.add_argument("--depends", action="store_true",
                        help="Print Debian Depends derived from the lock")
    args = parser.parse_args()
    if args.version == args.depends:
        parser.error("specify either --version or --depends")
    print(minimum_qca_version(args.lock) if args.version
          else qca_runtime_depends(args.lock))


if __name__ == "__main__":
    main()
