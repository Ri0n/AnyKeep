#!/usr/bin/env python3
"""Generate Qt plugin JSON metadata from a static descriptor and Qt TS catalogs."""

import argparse
import base64
import json
import mimetypes
import re
import sys
from pathlib import Path
import xml.etree.ElementTree as ET
from typing import Dict, Optional, Tuple


SCHEMA_VERSION = 2
KNOWN_FEATURES = {
    "regular",
    "desktopIntegration",
    "tray",
    "globalShortcuts",
    "notifications",
    "stickyNotes",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--translations-dir", required=True, type=Path)
    parser.add_argument("--icon", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qtnote-version", required=True)
    return parser.parse_args()


SEMVER_CORE_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
SEMVER_IDENTIFIER_RE = re.compile(r"^[0-9A-Za-z-]+$")


def semantic_version(value: object, current_version: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"semantic version must be a string: {value!r}")
    text = current_version.strip() if value.strip() == "current" else value.strip()
    if not text:
        raise ValueError(f"semantic version is empty: {value!r}")

    precedence, plus, build = text.partition("+")
    if plus:
        if "+" in build or not build:
            raise ValueError(f"invalid SemVer build metadata: {value!r}")
        build_ids = build.split(".")
        if any(not item or not SEMVER_IDENTIFIER_RE.fullmatch(item) for item in build_ids):
            raise ValueError(f"invalid SemVer build metadata: {value!r}")

    core, dash, prerelease = precedence.partition("-")
    if not SEMVER_CORE_RE.fullmatch(core):
        raise ValueError(f"invalid SemVer core version: {value!r}")
    if dash:
        if not prerelease:
            raise ValueError(f"empty SemVer prerelease: {value!r}")
        prerelease_ids = prerelease.split(".")
        for item in prerelease_ids:
            if not item or not SEMVER_IDENTIFIER_RE.fullmatch(item):
                raise ValueError(f"invalid SemVer prerelease: {value!r}")
            if item.isdigit() and len(item) > 1 and item.startswith("0"):
                raise ValueError(f"numeric SemVer prerelease has a leading zero: {value!r}")

    return text


def compare_semantic_versions(left: str, right: str) -> int:
    def parts(text: str):
        precedence = text.split("+", 1)[0]
        core, separator, prerelease = precedence.partition("-")
        return core.split("."), prerelease.split(".") if separator else []

    left_core, left_prerelease = parts(left)
    right_core, right_prerelease = parts(right)
    for left_item, right_item in zip(left_core, right_core):
        if len(left_item) != len(right_item):
            return -1 if len(left_item) < len(right_item) else 1
        if left_item != right_item:
            return -1 if left_item < right_item else 1

    if bool(left_prerelease) != bool(right_prerelease):
        return -1 if left_prerelease else 1
    for left_item, right_item in zip(left_prerelease, right_prerelease):
        left_numeric = left_item.isdigit()
        right_numeric = right_item.isdigit()
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        if left_numeric:
            if len(left_item) != len(right_item):
                return -1 if len(left_item) < len(right_item) else 1
        if left_item != right_item:
            return -1 if left_item < right_item else 1
    if len(left_prerelease) == len(right_prerelease):
        return 0
    return -1 if len(left_prerelease) < len(right_prerelease) else 1


def translation_text(message: ET.Element) -> Optional[str]:
    translation = message.find("translation")
    if translation is None:
        return None
    if translation.attrib.get("type") in {"unfinished", "vanished", "obsolete"}:
        return None
    text = "".join(translation.itertext()).strip()
    return text or None


def load_catalogs(directory: Path) -> Dict[str, Dict[str, Tuple[str, Optional[str]]]]:
    catalogs: Dict[str, Dict[str, Tuple[str, Optional[str]]]] = {}
    for path in sorted(directory.glob("plugin_metadata_*.ts")):
        root = ET.parse(path).getroot()
        language = root.attrib.get("language", "").strip().replace("-", "_")
        if not language:
            continue
        if language.lower().startswith("en"):
            language = "en"
        if language in catalogs:
            raise ValueError(f"duplicate TS language {language!r}: {path}")
        messages: Dict[str, Tuple[str, Optional[str]]] = {}
        for message in root.findall(".//message"):
            message_id = message.attrib.get("id", "").strip()
            if not message_id:
                continue
            if message_id in messages:
                raise ValueError(f"duplicate message id {message_id!r} in {path}")
            source = (message.findtext("source") or "").strip()
            messages[message_id] = (source, translation_text(message))
        catalogs[language] = messages
    if "en" not in catalogs:
        raise ValueError("plugin_metadata_en.ts is missing")
    return catalogs


def localized_value(
    catalogs: Dict[str, Dict[str, Tuple[str, Optional[str]]]], message_id: str
) -> Dict[str, str]:
    english = catalogs["en"].get(message_id)
    if english is None or not english[0]:
        raise ValueError(f"English source message {message_id!r} is missing")
    localized = {"en": english[0]}
    for language, messages in catalogs.items():
        if language == "en":
            continue
        message = messages.get(message_id)
        if message and message[0] == english[0] and message[1]:
            localized[language] = message[1]
    return localized


def main() -> int:
    args = parse_args()
    source = json.loads(args.source.read_text(encoding="utf-8"))
    catalogs = load_catalogs(args.translations_dir)

    name_id = source.pop("nameId")
    description_id = source.pop("descriptionId")
    source["schemaVersion"] = int(source.get("schemaVersion", SCHEMA_VERSION))
    if source["schemaVersion"] != SCHEMA_VERSION:
        raise ValueError(f"unsupported schemaVersion: {source['schemaVersion']}")
    if not str(source.get("id", "")).strip():
        raise ValueError("plugin id is empty")
    features = source.get("features")
    if not isinstance(features, list) or any(not isinstance(feature, str) for feature in features):
        raise ValueError("features must be an array of strings")
    unknown_features = sorted(set(features) - KNOWN_FEATURES)
    if unknown_features:
        raise ValueError(f"unknown plugin features: {', '.join(unknown_features)}")
    if len(features) != len(set(features)):
        raise ValueError("plugin features contain duplicates")

    desktop_environments = source.get("desktopEnvironments", [])
    if not isinstance(desktop_environments, list) or any(
        not isinstance(environment, str) or not environment.strip() for environment in desktop_environments
    ):
        raise ValueError("desktopEnvironments must be an array of non-empty strings")
    desktop_environments = [environment.strip().lower() for environment in desktop_environments]
    if len(desktop_environments) != len(set(desktop_environments)):
        raise ValueError("desktopEnvironments contains duplicates")
    if desktop_environments:
        source["desktopEnvironments"] = desktop_environments
    else:
        source.pop("desktopEnvironments", None)

    source["name"] = localized_value(catalogs, name_id)
    source["description"] = localized_value(catalogs, description_id)
    source["version"] = semantic_version(source["version"], args.qtnote_version)
    source["minVersion"] = semantic_version(source["minVersion"], args.qtnote_version)
    source["maxVersion"] = semantic_version(source["maxVersion"], args.qtnote_version)
    if compare_semantic_versions(source["minVersion"], source["maxVersion"]) > 0:
        raise ValueError("minVersion is greater than maxVersion")

    icon_bytes = args.icon.read_bytes()
    if not icon_bytes:
        raise ValueError(f"plugin icon is empty: {args.icon}")
    mime_type = mimetypes.guess_type(args.icon.name)[0] or "application/octet-stream"
    source["icon"] = {
        "mimeType": mime_type,
        "base64": base64.b64encode(icon_bytes).decode("ascii"),
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(source, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001 - build tool should print a concise error
        print(f"plugin metadata generation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
