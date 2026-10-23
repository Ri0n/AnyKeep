"""Single-source Qt toolchain/version policy from dependencies.lock.json.

The desktop/Android SDK Qt is intentionally independent of system Qt on Linux.
Keep the portable install-qt-action selector and package asset suffix derived
from one locked version, and reject mismatched QtKeychain release tags.
"""
import re


def qt_configuration(lock: dict) -> dict[str, str]:
    qt = lock["qt"]
    version = qt["toolchain_version"]
    match = re.fullmatch(r"([1-9][0-9]*)\.([0-9]+)\.\*", version)
    if not match:
        raise ValueError(f"qt.toolchain_version must be MAJOR.MINOR.*: {version!r}")

    series = f"{match.group(1)}.{match.group(2)}"
    sdk_suffix = f"qt{series}"
    toolchain_tuple = (int(match.group(1)), int(match.group(2)), 0)
    for key in ("minimum_mobile", "minimum_desktop"):
        minimum = qt[key]
        minimum_match = re.fullmatch(r"([1-9][0-9]*)\.([0-9]+)(?:\.([0-9]+))?", minimum)
        if not minimum_match:
            raise ValueError(f"qt.{key} must be a Qt version: {minimum!r}")
        minimum_tuple = tuple(int(part or 0) for part in minimum_match.groups())
        if minimum_tuple > toolchain_tuple:
            raise ValueError(f"qt.{key} {minimum} exceeds the toolchain {version}")

    keychain_tag = lock["qtkeychain"]["tag"]
    if not keychain_tag.endswith(f"-{sdk_suffix}"):
        raise ValueError(f"QtKeychain tag {keychain_tag!r} does not match Qt {series}")

    return {"version": version, "series": series, "sdk_suffix": sdk_suffix}
