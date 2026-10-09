#!/usr/bin/env python3
"""Inspect the Android APK's native payload and reject unsafe size regressions.

Only stdlib is used so the CI action can run this with the runner Python.
"""
import argparse
import os
from pathlib import Path
import sys
import zipfile


# Measured from the successful Android CI run for #142 (Qt 6.11, 2026-10-09).
BASELINE_APK_BYTES = {
    "arm64-v8a": 107699822,
    "x86_64": 111895846,
}


def inspect_apk(apk_path: Path, abi: str, linked_size: int) -> str:
    lib_prefix = f"lib/{abi}/"
    main_name = f"{lib_prefix}libanykeep_{abi}.so"
    with zipfile.ZipFile(apk_path) as archive:
        libraries = sorted(
            (item for item in archive.infolist()
             if item.filename.startswith(lib_prefix) and item.filename.endswith(".so")),
            key=lambda item: item.file_size,
            reverse=True,
        )
        lib_map = {item.filename: item for item in libraries}
        if main_name not in lib_map:
            raise ValueError(f"Missing application library: {main_name}")
        if lib_map[main_name].file_size != linked_size:
            raise ValueError("Packaged AnyKeep library differs from the verified linked ELF")
        if linked_size > 32 * 1024 * 1024:
            raise ValueError("Release application library is unexpectedly large")

        # Only the expected ABI is allowed; no accidental universal APK.
        other_abi_libs = [
            item.filename for item in archive.infolist()
            if item.filename.startswith("lib/")
            and item.filename.endswith(".so")
            and not item.filename.startswith(lib_prefix)
        ]
        if other_abi_libs:
            raise ValueError(f"Unexpected libraries from other ABIs: {other_abi_libs[:10]}")

        tooling = [
            name for name in lib_map if "/libplugins_qmltooling_" in name
        ]
        if tooling:
            raise ValueError(f"QML debug/profiling plugins still packaged: {tooling}")

        # These are runtime dependencies; stripping tooling must not eliminate
        # the Android platform, TLS, Material style, or multimedia support.
        required = (
            f"libplugins_platforms_qtforandroid_{abi}.so",
            f"libplugins_tls_qopensslbackend_{abi}.so",
            f"libplugins_multimedia_androidmediaplugin_{abi}.so",
            f"libqml_QtQuick_Controls_Material_qtquickcontrols2materialstyleplugin_{abi}.so",
            f"libqml_QtQuick_Controls_Basic_qtquickcontrols2basicstyleplugin_{abi}.so",
        )
        missing = [name for name in required if f"{lib_prefix}{name}" not in lib_map]
        if missing:
            raise ValueError(f"Essential Qt plugins missing from Android APK: {missing}")

        apk_size = apk_path.stat().st_size
        if apk_size > 140 * 1024 * 1024:
            raise ValueError("Release APK is unexpectedly large after stripping")

        styles = [
            item for item in libraries if "QtQuick_Controls_" in item.filename
            or "Qt6QuickControls2" in item.filename
        ]
        qml_plugins = [item for item in libraries if "/libqml_" in item.filename]
        native_plugins = [item for item in libraries if "/libplugins_" in item.filename]

        mib = 1024 * 1024
        baseline = BASELINE_APK_BYTES.get(abi)
        lines = [
            f"### Android APK inventory: {abi}",
            "",
            f"- APK: **{apk_size / mib:.2f} MiB**"
            + (f" (previous {baseline / mib:.2f} MiB; saved {(baseline - apk_size) / mib:.2f} MiB)" if baseline else ""),
            f"- Application ELF: **{linked_size / mib:.2f} MiB**",
            f"- Native shared objects: **{len(libraries)}**",
            f"- Qt runtime plugins: **{len(native_plugins)}**; QML tooling: **0**",
            f"- QML plugins: **{len(qml_plugins)}**",
            f"- Quick Controls and styles (uncompressed): **{sum(item.file_size for item in styles) / mib:.2f} MiB**",
            "",
            "Largest native objects (uncompressed):",
            "",
            "| Library | MiB |",
            "| --- | ---: |",
        ]
        for item in libraries[:15]:
            lines.append(f"| `{item.filename[len(lib_prefix):]}` | {item.file_size / mib:.2f} |")
        return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("abi", choices=tuple(BASELINE_APK_BYTES))
    parser.add_argument("linked_size", type=int)
    args = parser.parse_args()
    try:
        report = inspect_apk(args.apk, args.abi, args.linked_size)
    except (ValueError, OSError, zipfile.BadZipFile) as exc:
        print(f"Android APK verification failed: {exc}", file=sys.stderr)
        return 1
    print(report)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as output:
            output.write(report + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
