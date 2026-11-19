#!/usr/bin/env python3
"""Verify that the versioned libanykeep ELF in a .deb matches its package version.

The Debian changelog is synchronized from the release Git tag, but CMake can
independently resolve the library's VERSION property. A successful build must
not ship a 4.0.30 package containing libanykeep.so.4.0.5.
"""

import argparse
from pathlib import Path
import re
import subprocess

PACKAGE_VERSION = re.compile(r"^(?:[0-9]+:)?([0-9]+\.[0-9]+\.[0-9]+)(?:[~+.-].*)?$")
LIBRARY_FILENAME = re.compile(r"libanykeep\.so\.([0-9]+\.[0-9]+\.[0-9]+)$")


def verify(version: str, contents: str) -> str:
    match = PACKAGE_VERSION.fullmatch(version.strip())
    if not match:
        raise ValueError(f"Unsupported AnyKeep Debian package version: {version!r}")
    expected = match.group(1)

    libraries = []
    for line in contents.splitlines():
        columns = line.split()
        if len(columns) < 6 or not columns[0].startswith("-"):
            continue
        found = LIBRARY_FILENAME.fullmatch(Path(columns[5]).name)
        if found:
            libraries.append(found.group(1))
    if libraries != [expected]:
        raise ValueError(
            f"Package {version} must contain exactly libanykeep.so.{expected}; "
            f"found versioned libraries: {libraries}"
        )
    return expected


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    args = parser.parse_args()

    version = subprocess.check_output(
        ["dpkg-deb", "--field", str(args.package), "Version"], text=True
    ).strip()
    contents = subprocess.check_output(
        ["dpkg-deb", "--contents", str(args.package)], text=True
    )
    expected = verify(version, contents)
    print(f"Verified {args.package.name}: libanykeep.so.{expected} matches package {version}", flush=True)


if __name__ == "__main__":
    main()
