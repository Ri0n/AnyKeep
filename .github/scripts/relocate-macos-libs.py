#!/usr/bin/env python3
"""Relocate non-Qt shared libraries in a macOS app bundle.

macdeployqt deploys Qt frameworks, but does not reliably resolve @rpath
references to our already-staged AnyKeep, QCA and Iris dylibs. Use a
loader-relative reference for each consumer (executable, plugin or dylib).
"""
import os
import re
import subprocess
import sys
from pathlib import Path

MACHO_MAGICS = {
    bytes.fromhex(value)
    for value in (
        "feedface", "cefaedfe", "feedfacf", "cffaedfe",
        "cafebabe", "bebafeca", "cafebabf", "bfbafeca",
    )
}
OWN_DYLIB = re.compile(r"^lib(?:anykeep|qca3-qt6|iris-qt6)(?:\.\d+)*\.dylib$")
BUILD_ROOTS = ("/qca-sdk/", "/iris-sdk/", "/build/", "/stage/")
OPENSSL_PREFIXES = ("/opt/homebrew/opt/openssl", "/usr/local/opt/openssl")


def command(*args, check=True):
    return subprocess.run(args, check=check, capture_output=True, text=True)


def macho_files(bundle):
    for path in sorted(bundle.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as stream:
            if stream.read(4) in MACHO_MAGICS:
                yield path


def dependencies(binary):
    output = command("otool", "-L", str(binary)).stdout
    # otool can output multiple architecture headings for fat Mach-O binaries.
    result = set()
    for line in output.splitlines():
        line = line.strip()
        if " (compatibility version " in line:
            result.add(line.split(" (compatibility version ", 1)[0])
    return result


def install_id(binary):
    output = command("otool", "-D", str(binary), check=False)
    if output.returncode:
        return None
    return next(
        (line.strip() for line in output.stdout.splitlines()
         if line.strip() and not line.endswith(":")),
        None,
    )


def relocate(bundle):
    frameworks = bundle / "Contents" / "Frameworks"
    changes = 0
    seen = set()
    for binary in macho_files(bundle):
        own_id = install_id(binary)
        for dependency in sorted(dependencies(binary)):
            name = dependency.rsplit("/", 1)[-1]
            if not OWN_DYLIB.fullmatch(name):
                continue
            target = frameworks / name
            if not target.is_file():
                raise RuntimeError(f"{binary}: missing bundled library {target}")
            seen.add(name)
            if dependency == own_id:
                continue  # LC_ID_DYLIB is not an LC_LOAD_DYLIB reference.
            relative = os.path.relpath(target, binary.parent).replace(os.sep, "/")
            replacement = f"@loader_path/{relative}"
            if dependency != replacement:
                command("install_name_tool", "-change", dependency, replacement, str(binary))
                changes += 1

    if not seen:
        raise RuntimeError("No AnyKeep, QCA or Iris dylib references found in bundle")
    print(f"Relocated {changes} load commands for {len(seen)} bundled library names", flush=True)


def verify(bundle):
    frameworks = (bundle / "Contents" / "Frameworks").resolve()
    inspected = 0
    own_refs = 0
    failures = []
    for binary in macho_files(bundle):
        inspected += 1
        own_id = install_id(binary)
        for dependency in sorted(dependencies(binary)):
            if dependency == own_id:
                continue
            name = dependency.rsplit("/", 1)[-1]
            if OWN_DYLIB.fullmatch(name):
                own_refs += 1
                if not dependency.startswith("@loader_path/"):
                    failures.append(f"{binary}: unrelocated {dependency}")
                    continue
                resolved = (binary.parent / dependency[len("@loader_path/"):]).resolve()
                if not resolved.is_file() or not resolved.is_relative_to(frameworks):
                    failures.append(f"{binary}: broken library path {dependency}")
            if dependency.startswith(OPENSSL_PREFIXES) or (
                dependency.startswith("/") and any(root in dependency for root in BUILD_ROOTS)
            ):
                failures.append(f"{binary}: build-machine dependency {dependency}")
    if own_refs == 0:
        failures.append("No AnyKeep/QCA/Iris consumers found")
    if failures:
        raise RuntimeError("Non-relocatable macOS bundle:\n" + "\n".join(failures))
    print(f"Verified {own_refs} relocated references in {inspected} Mach-O files", flush=True)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("patch", "verify"):
        raise SystemExit("Usage: relocate-macos-libs.py {patch|verify} <path-to.app>")
    bundle = Path(sys.argv[2]).resolve()
    if not (bundle / "Contents" / "Frameworks").is_dir():
        raise SystemExit(f"Missing app frameworks directory: {bundle}")
    if sys.argv[1] == "patch":
        relocate(bundle)
    else:
        verify(bundle)


if __name__ == "__main__":
    main()
