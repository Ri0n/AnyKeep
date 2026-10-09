#!/usr/bin/env python3
"""Limit androiddeployqt's QML import scan to Material and Basic controls styles.

Qt's qmlimportscanner reports all runtime-selectable Quick Controls styles on
Android. This wrapper delegates scanning to the exact Qt host tool, then removes
only the unused style modules from the *deployment* import list. Normal QML
compiler scanning, ELF dependency detection, Qt platform plugins, multimedia,
TLS, Material and its Basic fallback remain unchanged.

Configuration is intentionally scoped to the shared Android Release CI action.
"""
import json
import os
from pathlib import Path
import subprocess
import sys


# Each name is a *complete* QML module name, not a prefix. Do not exclude
# QtQuick.Controls itself, its implementations or the Material/Basic styles.
UNUSED_STYLE_NAMES = frozenset(
    f"QtQuick.Controls.{style}{suffix}"
    for style in ("Fusion", "Imagine", "Universal", "FluentWinUI3")
    for suffix in ("", ".impl")
)


def filter_imports(imports):
    if not isinstance(imports, list):
        raise ValueError("Qt qmlimportscanner must produce a JSON array")
    filtered = []
    removed = []
    for entry in imports:
        if not isinstance(entry, dict):
            raise ValueError("Qt qmlimportscanner entry must be an object")
        if entry.get("name") in UNUSED_STYLE_NAMES and entry.get("type") == "module":
            removed.append(entry.get("name"))
        else:
            filtered.append(entry)
    return filtered, removed


def configure(settings_path: Path) -> int:
    with settings_path.open(encoding="utf-8") as file:
        settings = json.load(file)
    # Qt 6.11 deployment settings do not necessarily provide qtHostDir.
    # The aqtinstall action installs Android and host Qt kits as siblings.
    qt_root = Path(os.environ.get("QT_ROOT_DIR", "/not/a/qt/dir"))
    host_directory = settings.get("qtHostDir")
    candidates = []
    if settings.get("qml-importscanner-binary"):
        candidates.append(Path(settings["qml-importscanner-binary"]))
    if settings.get("qtLibExecsDirectory"):
        candidates.append(Path(settings["qtLibExecsDirectory"]) / "qmlimportscanner")
    if host_directory:
        host_dir = Path(host_directory)
        candidates.extend(
            [host_dir / "libexec/qmlimportscanner", host_dir / "bin/qmlimportscanner"]
        )
    for sibling in ("gcc_64", "clang_64", "linux_gcc_64"):
        candidates.extend([
            qt_root.parent / sibling / "libexec/qmlimportscanner",
            qt_root.parent / sibling / "bin/qmlimportscanner",
        ])
    # Additional host kit names are allowed without depending on Qt's internal
    # JSON schema or selecting a target-architecture executable by mistake.
    candidates.extend(qt_root.parent.glob("*/libexec/qmlimportscanner"))
    real_scanner = next(
        (p for p in candidates if p.is_file() and os.access(p, os.X_OK)
         and p.resolve() != Path(__file__).resolve()),
        None,
    )
    if real_scanner is None:
        raise ValueError(
            "Cannot find the Qt host qmlimportscanner; "
            f"deployment JSON keys: {sorted(settings)}, candidates: {candidates}"
        )

    wrapper = Path(__file__).resolve()
    settings["qml-importscanner-binary"] = str(wrapper)
    with settings_path.open("w", encoding="utf-8") as file:
        json.dump(settings, file, indent=2)
        file.write("\n")
    # Printed path is captured and exported by the current CI shell.\n    print(real_scanner.resolve())
    return 0


def scan(args) -> int:
    binary = os.environ.get("ANYKEEP_QMLIMPORTSCANNER_REAL")
    if not binary or not Path(binary).is_file():
        raise ValueError("ANYKEEP_QMLIMPORTSCANNER_REAL is missing or invalid")
    proc = subprocess.run([binary, *args], capture_output=True, text=True, check=False)
    if proc.stderr:
        sys.stderr.write(proc.stderr)
    if proc.returncode:
        sys.stdout.write(proc.stdout)
        return proc.returncode
    data = json.loads(proc.stdout)
    filtered, removed = filter_imports(data)
    print(json.dumps(filtered))
    print(f"AnyKeep Android QML scan: excluded {len(removed)} style imports: {', '.join(sorted(set(removed)))}", file=sys.stderr)
    return 0


def main():
    try:
        if len(sys.argv) == 3 and sys.argv[1] == "--configure":
            return configure(Path(sys.argv[2]))
        return scan(sys.argv[1:])
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"Android QML import filter failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
