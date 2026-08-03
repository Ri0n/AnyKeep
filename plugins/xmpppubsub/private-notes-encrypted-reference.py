#!/usr/bin/env python3
"""Independent AnyKeep encrypted PubSub XML reference codec.

The protocol uses one major-version namespace for PubSub node names, the outer
encrypted element, and authenticated plaintext. XML serialization is not
canonical: implementations validate semantic content after AES-256-GCM decrypt.
"""

from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import hmac
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape, quoteattr
from pathlib import Path
from typing import Any, Iterable

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

PUBSUB_NS = "http://jabber.org/protocol/pubsub"
PROTOCOL_NS = "urn:xmpp:private-notes:0"
FOLDER_NS = "urn:xmpp:private-notes:folders:0"
CONTENT_REVISION_NS = "urn:xmpp:private-notes:content:0"
LEGACY_ENCRYPTED_NS = "urn:xmpp:private-notes:encrypted:0"
MAX_XML_SIZE = 16 * 1024 * 1024
HKDF_SALT = b"private-notes HKDF salt v1"
HKDF_INFO_PREFIX = b"private-notes key domain v1:"
KEY_ID_PREFIX = b"private-notes storage key id v1\0"

ET.register_namespace("pn", PROTOCOL_NS)
ET.register_namespace("folder", FOLDER_NS)
ET.register_namespace("content-revision", CONTENT_REVISION_NS)
ET.register_namespace("legacy", LEGACY_ENCRYPTED_NS)


class ProtocolError(Exception):
    def __init__(self, category: str, message: str):
        super().__init__(message)
        self.category = category
        self.message = message


def fail(category: str, message: str) -> None:
    raise ProtocolError(category, message)


def qname(local: str, namespace: str = PROTOCOL_NS) -> str:
    return f"{{{namespace}}}{local}"


def hex_bytes(value: Any, name: str, expected_size: int | None = None) -> bytes:
    if not isinstance(value, str):
        fail("invalid_argument", f"{name} must be hexadecimal text")
    try:
        decoded = bytes.fromhex(value)
    except ValueError as error:
        fail("invalid_argument", f"invalid {name}: {error}")
    if expected_size is not None and len(decoded) != expected_size:
        fail("invalid_argument", f"{name} must contain {expected_size} bytes")
    return decoded


def storage_key_id(master_key: bytes) -> bytes:
    if len(master_key) != 32:
        fail("invalid_argument", "master key must contain 32 bytes")
    return hashlib.sha256(KEY_ID_PREFIX + master_key).digest()


def derive_key(master_key: bytes, kind: str) -> bytes:
    if len(master_key) != 32:
        fail("invalid_argument", "master key must contain 32 bytes")
    domain = {"index": "storage-index", "content": "storage-content"}.get(kind)
    if domain is None:
        fail("invalid_argument", "kind must be index or content")
    prk = hmac.new(HKDF_SALT, master_key, hashlib.sha256).digest()
    info = HKDF_INFO_PREFIX + domain.encode("ascii")
    return hmac.new(prk, info + b"\x01", hashlib.sha256).digest()


def b64(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def decode_b64(text: str, name: str, maximum: int) -> bytes:
    compact = "".join((text or "").split())
    if not compact or len(compact) % 4 == 1 or len(compact) > ((maximum + 2) // 3) * 4:
        fail("malformed", f"invalid {name} Base64 length")
    try:
        decoded = base64.b64decode(compact, validate=True)
    except Exception as error:
        fail("malformed", f"invalid {name} Base64: {error}")
    if len(decoded) > maximum or b64(decoded) != compact:
        fail("malformed", f"non-canonical {name} Base64")
    return decoded


def decode_b64url(text: str, name: str, size: int) -> bytes:
    if not re.fullmatch(r"[A-Za-z0-9_-]+", text or "") or "=" in (text or ""):
        fail("malformed", f"invalid {name} Base64url")
    padded = text + "=" * ((4 - len(text) % 4) % 4)
    try:
        decoded = base64.urlsafe_b64decode(padded)
    except Exception as error:
        fail("malformed", f"invalid {name} Base64url: {error}")
    if len(decoded) != size or b64url(decoded) != text:
        fail("malformed", f"non-canonical {name} Base64url")
    return decoded


def parse_xml(data: bytes, name: str) -> ET.Element:
    if not data or len(data) > MAX_XML_SIZE:
        fail("malformed" if not data else "unsupported", f"invalid {name} size")
    upper = data.upper()
    if b"<!DOCTYPE" in upper or b"<!ENTITY" in upper:
        fail("malformed", f"document types and entity declarations are forbidden in {name}")
    try:
        return ET.fromstring(data)
    except ET.ParseError as error:
        fail("malformed", f"invalid {name} XML: {error}")


def direct_children(parent: ET.Element, local: str, namespace: str = PROTOCOL_NS) -> list[ET.Element]:
    return [child for child in list(parent) if child.tag == qname(local, namespace)]


def simple_text(element: ET.Element, name: str) -> str:
    if list(element):
        fail("malformed", f"{name} must not contain child elements")
    return element.text or ""


def direct_text_is_whitespace(element: ET.Element) -> bool:
    if element.text and element.text.strip():
        return False
    return all(not child.tail or not child.tail.strip() for child in list(element))


def split_tag(tag: str) -> tuple[str, str]:
    if tag.startswith("{"):
        namespace, local = tag[1:].split("}", 1)
        return namespace, local
    return "", tag


def validate_attributes(element: ET.Element, allowed_core: set[str], context: str,
                        allow_foreign: bool = True) -> None:
    for name in element.attrib:
        namespace, local = split_tag(name)
        if not namespace:
            if local not in allowed_core:
                fail("malformed", f"unknown core attribute in {context}")
        elif namespace == PROTOCOL_NS or not allow_foreign:
            fail("malformed", f"unsupported attribute namespace in {context}")


def validate_children(element: ET.Element, allowed_core: set[str], context: str) -> None:
    for child in list(element):
        namespace, local = split_tag(child.tag)
        if not namespace:
            fail("malformed", f"unnamespaced child element in {context}")
        if namespace == PROTOCOL_NS and local not in allowed_core:
            fail("malformed", f"unknown core element in {context}")


def validate_leaf(element: ET.Element, context: str) -> None:
    validate_attributes(element, set(), context, allow_foreign=False)
    if list(element):
        fail("malformed", f"{context} must not contain child elements")


def folder_path(record: ET.Element) -> list[str]:
    folders = direct_children(record, "folder", FOLDER_NS)
    if len(folders) > 1:
        fail("malformed", "index contains more than one folder path")
    if not folders:
        return []

    folder = folders[0]
    validate_attributes(folder, set(), "folder", allow_foreign=False)
    if not direct_text_is_whitespace(folder):
        fail("malformed", "unexpected text in folder")
    segments = direct_children(folder, "segment", FOLDER_NS)
    if not segments:
        fail("malformed", "folder must contain one or more segments")
    for child in list(folder):
        namespace, local = split_tag(child.tag)
        if namespace != FOLDER_NS or local != "segment":
            fail("malformed", "invalid child in folder")

    result = []
    for segment in segments:
        validate_leaf(segment, "folder segment")
        value = simple_text(segment, "folder segment")
        if not value or value != value.strip():
            fail("malformed", "folder segments must be non-empty and trimmed")
        result.append(value)
    return result


def content_revision(record: ET.Element, index_revision: str, required_features: set[str]) -> str:
    values = direct_children(record, "content-revision", CONTENT_REVISION_NS)
    required = CONTENT_REVISION_NS in required_features
    if not values:
        if required:
            fail("malformed", "required content-revision extension is missing from index")
        return index_revision
    if len(values) != 1:
        fail("malformed", "index contains more than one content revision")
    if not required:
        fail("malformed", "content-revision extension must be declared as required")
    validate_leaf(values[0], "content revision")
    value = simple_text(values[0], "content revision")
    if not value:
        fail("malformed", "content revision must be non-empty")
    if value == index_revision:
        fail("malformed", "content revision extension must differ from index revision")
    return value


def build_plaintext(request: dict[str, Any]) -> bytes:
    supplied = request.get("plaintext_xml")
    if supplied is not None:
        if not isinstance(supplied, str):
            fail("invalid_argument", "plaintext_xml must be text")
        data = supplied.encode("utf-8")
        parse_xml(data, "provided plaintext")
        return data

    kind = request.get("kind")
    item_id = request.get("item_id")
    node = request.get("node")
    record = request.get("record")
    if kind not in ("index", "content") or not isinstance(item_id, str) or not item_id:
        fail("invalid_argument", "kind and item_id are required")
    if not isinstance(node, str) or not node or not isinstance(record, dict):
        fail("invalid_argument", "node and record are required")

    if kind == "index":
        revision = record.get("revision")
        content_revision_value = record.get("content_revision", revision)
        modified = record.get("modified")
        fmt = record.get("format", "markdown")
        if not all(isinstance(v, str) and v for v in (revision, content_revision_value, modified, fmt)):
            fail("invalid_argument", "index revision, modified and format are required")
        attributes = [f"id={quoteattr(item_id)}", f"revision={quoteattr(revision)}",
                      f"modified={quoteattr(modified)}", f"format={quoteattr(fmt)}"]
        for source, target in (("parent_revision", "parent-revision"), ("origin_id", "origin-id")):
            value = record.get(source)
            if value is not None:
                if not isinstance(value, str) or not value:
                    fail("invalid_argument", f"{source} must be non-empty text")
                attributes.append(f"{target}={quoteattr(value)}")
        tags = record.get("tags", [])
        if not isinstance(tags, list) or not all(isinstance(tag, str) for tag in tags):
            fail("invalid_argument", "tags must be an array of strings")
        folder = record.get("folder_path", [])
        if not isinstance(folder, list) or not all(isinstance(segment, str) and segment and segment == segment.strip()
                                                    for segment in folder):
            fail("invalid_argument", "folder_path must be an array of non-empty trimmed strings")
        children = f"<title>{escape(str(record.get('title', '')))}</title>" + "".join(
            f"<tag>{escape(tag)}</tag>" for tag in tags)
        if folder:
            children += f"<folder xmlns={quoteattr(FOLDER_NS)}>" + "".join(
                f"<segment>{escape(segment)}</segment>" for segment in folder
            ) + "</folder>"
        required_xml = ""
        if content_revision_value != revision:
            required_xml = f"<required feature={quoteattr(CONTENT_REVISION_NS)}/>"
            children += (f"<content-revision xmlns={quoteattr(CONTENT_REVISION_NS)}>"
                         f"{escape(content_revision_value)}</content-revision>")
        record_xml = f"<index {' '.join(attributes)}>{children}</index>"
    else:
        revision = record.get("revision")
        if not isinstance(revision, str) or not revision:
            fail("invalid_argument", "content revision is required")
        body = escape(str(record.get("body", "")))
        record_xml = f"<note id={quoteattr(item_id)} revision={quoteattr(revision)}><body>{body}</body></note>"

    return (f"<envelope xmlns={quoteattr(PROTOCOL_NS)}><node>{escape(node)}</node>{required_xml if kind == 'index' else ''}"
            f"<content>{record_xml}</content></envelope>").encode("utf-8")


def build_outer(item_id: str, key_id: bytes, nonce: bytes, ciphertext: bytes, tag: bytes) -> str:
    return (f"<item id={quoteattr(item_id)}><encrypted xmlns={quoteattr(PROTOCOL_NS)} "
            f"key-id={quoteattr(b64url(key_id))}><nonce>{b64(nonce)}</nonce>"
            f"<payload>{b64(ciphertext)}</payload><tag>{b64(tag)}</tag></encrypted></item>")


def parse_outer(xml_bytes: bytes) -> dict[str, Any]:
    root = parse_xml(xml_bytes, "PubSub item")
    if root.tag not in ("item", f"{{{PUBSUB_NS}}}item"):
        fail("malformed", "outer root must be a PubSub item")
    item_id = root.attrib.get("id", "")
    children = list(root)
    if not item_id or len(children) != 1:
        fail("malformed", "PubSub item ID or encrypted element is missing")
    encrypted = children[0]
    if encrypted.tag == qname("encrypted", LEGACY_ENCRYPTED_NS):
        fail("obsolete", "obsolete pre-unified AnyKeep encrypted payload")
    if encrypted.tag != qname("encrypted"):
        namespace = encrypted.tag[1:].split("}", 1)[0] if encrypted.tag.startswith("{") else ""
        if namespace.startswith("urn:xmpp:private-notes:"):
            fail("unsupported", "unsupported AnyKeep protocol major namespace")
        fail("malformed", "unexpected encrypted payload namespace")
    validate_attributes(encrypted, {"key-id"}, "encrypted AnyKeep payload")
    validate_children(encrypted, {"nonce", "payload", "tag"}, "encrypted AnyKeep payload")
    if not direct_text_is_whitespace(encrypted):
        fail("malformed", "unexpected direct text in encrypted element")
    key_id = decode_b64url(encrypted.attrib.get("key-id", ""), "key ID", 32)

    def one(local: str, maximum: int) -> bytes:
        fields = direct_children(encrypted, local)
        if len(fields) != 1:
            fail("malformed", f"expected one simple {local} element")
        validate_leaf(fields[0], local)
        return decode_b64(fields[0].text or "", local, maximum)

    nonce, ciphertext, tag = one("nonce", 12), one("payload", MAX_XML_SIZE), one("tag", 16)
    if len(nonce) != 12 or not ciphertext or len(tag) != 16:
        fail("malformed", "invalid AES-GCM envelope sizes")
    return {"item_id": item_id, "key_id": key_id, "nonce": nonce, "ciphertext": ciphertext, "tag": tag}


def validate_plaintext(plaintext: bytes, kind: str, actual_node: str, item_id: str,
                       supported_features: Iterable[str] = ()) -> dict[str, Any]:
    root = parse_xml(plaintext, "authenticated plaintext")
    if root.tag != qname("envelope"):
        namespace = root.tag[1:].split("}", 1)[0] if root.tag.startswith("{") else ""
        if namespace.startswith("urn:xmpp:private-notes:"):
            fail("unsupported", "unsupported authenticated protocol major namespace")
        fail("malformed", "authenticated root must be envelope")
    validate_attributes(root, set(), "authenticated AnyKeep envelope")
    validate_children(root, {"node", "required", "content"}, "authenticated AnyKeep envelope")
    if not direct_text_is_whitespace(root):
        fail("malformed", "unexpected text in authenticated AnyKeep envelope")
    nodes = direct_children(root, "node")
    contents = direct_children(root, "content")
    if len(nodes) != 1 or len(contents) != 1:
        fail("malformed", "envelope must contain one node and one content")
    validate_leaf(nodes[0], "node")
    if simple_text(nodes[0], "node") != actual_node:
        fail("context_mismatch", "authenticated node binding differs")
    supported = {CONTENT_REVISION_NS, *supported_features}
    required_features: set[str] = set()
    for required in direct_children(root, "required"):
        if set(required.attrib) != {"feature"} or list(required) or (required.text or "").strip():
            fail("malformed", "invalid required extension declaration")
        feature = required.attrib["feature"]
        if feature in required_features:
            fail("malformed", "duplicate required extension declaration")
        required_features.add(feature)
        if feature not in supported:
            fail("unsupported", f"unsupported required extension {feature}")
    if kind != "index" and CONTENT_REVISION_NS in required_features:
        fail("malformed", "content-revision extension is valid only for an index record")
    validate_attributes(contents[0], set(), "authenticated AnyKeep content")
    validate_children(contents[0], {"index", "note"}, "authenticated AnyKeep content")
    if not direct_text_is_whitespace(contents[0]):
        fail("malformed", "unexpected text in authenticated AnyKeep content")
    expected = "index" if kind == "index" else "note"
    records = direct_children(contents[0], expected)
    other = direct_children(contents[0], "note" if expected == "index" else "index")
    if len(records) != 1 or other:
        fail("malformed", f"content must contain exactly one {expected}")
    record = records[0]
    if record.attrib.get("id") != item_id:
        fail("context_mismatch", "record ID differs from PubSub item ID")
    revision = record.attrib.get("revision", "")
    if not revision:
        fail("malformed", "record revision is missing")
    if kind == "index":
        validate_attributes(record, {"id", "revision", "parent-revision", "origin-id", "modified", "format"},
                            "authenticated AnyKeep index")
        validate_children(record, {"title", "tag"}, "authenticated AnyKeep index")
        if not direct_text_is_whitespace(record):
            fail("malformed", "unexpected text in authenticated AnyKeep index")
        if record.attrib.get("format") != "markdown" or not record.attrib.get("modified"):
            fail("unsupported" if record.attrib.get("format") not in (None, "markdown") else "malformed",
                 "invalid index format or modified time")
        titles = direct_children(record, "title")
        if len(titles) != 1:
            fail("malformed", "index must contain one title")
        validate_leaf(titles[0], "title")
        for tag in direct_children(record, "tag"):
            validate_leaf(tag, "tag")
        path = folder_path(record)
        body_revision = content_revision(record, revision, required_features)
        return {
            "kind": kind, "id": item_id, "revision": revision,
            "content_revision": body_revision,
            "parent_revision": record.attrib.get("parent-revision", ""),
            "origin_id": record.attrib.get("origin-id", ""),
            "title": simple_text(titles[0], "title"),
            "modified": record.attrib["modified"], "format": "markdown",
            "tags": [simple_text(tag, "tag") for tag in direct_children(record, "tag")],
            "folder_path": path,
        }
    validate_attributes(record, {"id", "revision"}, "authenticated AnyKeep content record")
    validate_children(record, {"body"}, "authenticated AnyKeep content record")
    if not direct_text_is_whitespace(record):
        fail("malformed", "unexpected text in authenticated AnyKeep content record")
    bodies = direct_children(record, "body")
    if len(bodies) != 1:
        fail("malformed", "note must contain one body")
    validate_leaf(bodies[0], "body")
    return {"kind": kind, "id": item_id, "revision": revision, "body": simple_text(bodies[0], "body")}


def encode_request(request: dict[str, Any]) -> dict[str, Any]:
    kind = request.get("kind")
    node = request.get("node")
    item_id = request.get("item_id")
    if kind not in ("index", "content") or not isinstance(node, str) or not node or not isinstance(item_id, str) or not item_id:
        fail("invalid_argument", "kind, node and item_id are required")
    master_key = hex_bytes(request.get("master_key_hex"), "master key", 32)
    nonce = hex_bytes(request["nonce_hex"], "nonce", 12) if "nonce_hex" in request else os.urandom(12)
    plaintext = build_plaintext(request)
    key_id = storage_key_id(master_key)
    derived = derive_key(master_key, kind)
    combined = AESGCM(derived).encrypt(nonce, plaintext, None)
    ciphertext, tag = combined[:-16], combined[-16:]
    xml = build_outer(item_id, key_id, nonce, ciphertext, tag)
    return {
        "protocol_namespace": PROTOCOL_NS,
        "kind": kind,
        "master_key_hex": master_key.hex(),
        "node": node,
        "item_id": item_id,
        "key_id_hex": key_id.hex(),
        "key_id_base64url": b64url(key_id),
        "derived_key_hex": derived.hex(),
        "nonce_hex": nonce.hex(),
        "ciphertext_hex": ciphertext.hex(),
        "tag_hex": tag.hex(),
        "plaintext_xml": plaintext.decode("utf-8"),
        "plaintext_hex": plaintext.hex(),
        "xml": xml,
    }


def decode_document(document: dict[str, Any], master_key_override: str | None = None,
                    node_override: str | None = None, supported_features: Iterable[str] = ()) -> dict[str, Any]:
    kind = document.get("kind")
    if kind not in ("index", "content"):
        fail("invalid_argument", "encoded document kind must be index or content")
    master_key = hex_bytes(master_key_override or document.get("master_key_hex"), "master key", 32)
    node = node_override or document.get("node")
    if not isinstance(node, str) or not node:
        fail("invalid_argument", "actual PubSub node is required")
    outer = parse_outer(str(document.get("xml", "")).encode("utf-8"))
    expected_key_id = storage_key_id(master_key)
    if outer["key_id"] != expected_key_id:
        fail("wrong_key", "configured storage key ID differs")
    try:
        plaintext = AESGCM(derive_key(master_key, kind)).decrypt(
            outer["nonce"], outer["ciphertext"] + outer["tag"], None)
    except Exception:
        fail("authentication_failed", "AES-GCM authentication failed")
    record = validate_plaintext(plaintext, kind, node, outer["item_id"], supported_features)
    return {
        "protocol_namespace": PROTOCOL_NS,
        "kind": kind,
        "node": node,
        "item_id": outer["item_id"],
        "key_id_hex": outer["key_id"].hex(),
        "plaintext_xml": plaintext.decode("utf-8"),
        "record": record,
    }


def replace_field(xml: str, local: str, value: bytes, canonical: bool = True) -> str:
    root = ET.fromstring(xml)
    encrypted = list(root)[0]
    field = direct_children(encrypted, local)[0]
    field.text = b64(value) if canonical else base64.b64encode(value).decode("ascii").rstrip("=")
    return ET.tostring(root, encoding="unicode")


def reencrypt_plaintext(encoded: dict[str, Any], plaintext: bytes) -> dict[str, Any]:
    master = bytes.fromhex(encoded["master_key_hex"])
    nonce = bytes.fromhex(encoded["nonce_hex"])
    combined = AESGCM(derive_key(master, encoded["kind"])).encrypt(nonce, plaintext, None)
    encoded["ciphertext_hex"], encoded["tag_hex"] = combined[:-16].hex(), combined[-16:].hex()
    encoded["plaintext_xml"] = plaintext.decode("utf-8", errors="replace")
    encoded["plaintext_hex"] = plaintext.hex()
    encoded["xml"] = build_outer(encoded["item_id"], bytes.fromhex(encoded["key_id_hex"]), nonce,
                                 combined[:-16], combined[-16:])
    return encoded


def reference_requests() -> list[tuple[str, str, dict[str, Any]]]:
    key = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    item = "2b7e1516-28ae-4d2a-abf7-158809cf4f3c"
    return [
        ("index", "Complete index record", {
            "kind": "index", "master_key_hex": key, "node": f"{PROTOCOL_NS}:index", "item_id": item,
            "nonce_hex": "000102030405060708090a0b",
            "record": {"revision": "018f0be0-df0d-7c70-a2ef-1f5c973ec92a", "origin_id": "device-a",
                       "title": "Portable note", "modified": "2026-07-27T18:00:00.123Z",
                       "format": "markdown", "tags": ["one", "two"],
                       "folder_path": ["Projects", "2026"]},
        }),
        ("index-metadata-only", "Metadata-only index update bound to an existing body", {
            "kind": "index", "master_key_hex": key, "node": f"{PROTOCOL_NS}:index", "item_id": item,
            "nonce_hex": "101112131415161718191a1b",
            "record": {"revision": "folder-revision", "content_revision": "body-revision",
                       "parent_revision": "body-revision", "origin_id": "device-a", "title": "Portable note",
                       "modified": "2026-07-27T18:01:00.123Z", "format": "markdown", "tags": ["one"],
                       "folder_path": ["Archive"]},
        }),
        ("content", "Complete content record", {
            "kind": "content", "master_key_hex": key, "node": f"{PROTOCOL_NS}:content", "item_id": item,
            "nonce_hex": "0c0d0e0f1011121314151617",
            "record": {"revision": "018f0be0-df0d-7c70-a2ef-1f5c973ec92a", "body": "Portable body\n"},
        }),
        ("index-extensions", "Unicode and optional foreign-namespace extensions", {
            "kind": "index", "master_key_hex": key, "node": f"{PROTOCOL_NS}:index", "item_id": "unicode-note",
            "nonce_hex": "18191a1b1c1d1e1f20212223",
            "plaintext_xml": (
                f'<envelope xmlns="{PROTOCOL_NS}" xmlns:x="urn:example:private-notes:extension:1" x:root="preserve">'
                f'<node>{PROTOCOL_NS}:index</node><x:envelope value="42"/><content x:box="yes">'
                '<index id="unicode-note" revision="revision-unicode" modified="2026-07-27T18:00:00.000Z" '
                'format="markdown" x:record="keep"><title>Привет &amp; café</title><tag>тест</tag>'
                '<x:record>future</x:record></index></content></envelope>'
            ),
        }),
        ("index-minimal", "Empty title and no optional index fields", {
            "kind": "index", "master_key_hex": key, "node": f"{PROTOCOL_NS}:index", "item_id": "minimal-note",
            "nonce_hex": "2425262728292a2b2c2d2e2f",
            "record": {"revision": "minimal-revision", "title": "", "modified": "2026-07-27T18:00:00Z",
                       "format": "markdown", "tags": []},
        }),
        ("content-empty", "Empty note body", {
            "kind": "content", "master_key_hex": key, "node": f"{PROTOCOL_NS}:content", "item_id": "empty-note",
            "nonce_hex": "303132333435363738393a3b",
            "record": {"revision": "empty-revision", "body": ""},
        }),
    ]


def generate_vectors() -> dict[str, Any]:
    positives = []
    for name, description, request in reference_requests():
        encoded = encode_request(request)
        positives.append({"name": name, "description": description, "request": request,
                          "encoded": encoded, "expected_decoded": decode_document(encoded)})
    base = positives[0]["encoded"]
    negatives: list[dict[str, Any]] = []

    def add(name: str, description: str, category: str, encoded: dict[str, Any]) -> None:
        negatives.append({"name": name, "description": description,
                          "expected_error": category, "encoded": encoded})

    value = copy.deepcopy(base)
    root = ET.fromstring(value["xml"])
    encrypted = list(root)[0]
    encrypted.tag = qname("encrypted", LEGACY_ENCRYPTED_NS)
    value["xml"] = ET.tostring(root, encoding="unicode")
    add("obsolete-legacy-namespace", "Pre-unified encrypted namespace", "obsolete", value)

    value = copy.deepcopy(base); value["master_key_hex"] = "55" * 32
    add("wrong-key", "Configured master key differs", "wrong_key", value)
    value = copy.deepcopy(base); value["node"] = f"{PROTOCOL_NS}:another-index"
    add("wrong-node", "Authenticated node differs", "context_mismatch", value)
    value = copy.deepcopy(base)
    root = ET.fromstring(value["xml"]); root.set("id", "moved-item"); value["xml"] = ET.tostring(root, encoding="unicode")
    add("wrong-item-id", "Record ID differs from outer item ID", "context_mismatch", value)
    value = copy.deepcopy(base); tag = bytearray.fromhex(value["tag_hex"]); tag[0] ^= 1
    value["xml"] = replace_field(value["xml"], "tag", bytes(tag))
    add("tampered-tag", "AES-GCM tag was modified", "authentication_failed", value)
    value = copy.deepcopy(base)
    root = ET.fromstring(value["xml"]); enc = list(root)[0]; enc.remove(direct_children(enc, "tag")[0]); value["xml"] = ET.tostring(root, encoding="unicode")
    add("missing-tag", "Outer envelope is incomplete", "malformed", value)
    value = copy.deepcopy(base); value["xml"] = replace_field(value["xml"], "payload", bytes.fromhex(value["ciphertext_hex"]), False)
    add("noncanonical-base64", "Base64 is not canonical", "malformed", value)
    plaintext = base["plaintext_xml"].replace("<index ", '<index minor="1" ', 1).encode()
    value = reencrypt_plaintext(copy.deepcopy(base), plaintext)
    add("unknown-core-attribute", "Unknown unqualified core attribute", "malformed", value)
    plaintext = base["plaintext_xml"].replace("</index>", "<future/></index>", 1).encode()
    value = reencrypt_plaintext(copy.deepcopy(base), plaintext)
    add("unknown-core-element", "Unknown element in the current core namespace", "malformed", value)
    plaintext = base["plaintext_xml"].replace(
        "<segment>Projects</segment>", "<segment> </segment>", 1).encode()
    value = reencrypt_plaintext(copy.deepcopy(base), plaintext)
    add("untrimmed-folder-segment", "Folder segments must be non-empty and trimmed", "malformed", value)
    plaintext = base["plaintext_xml"].replace(
        "</folder>",
        "</folder><folder xmlns=\"urn:xmpp:private-notes:folders:0\"><segment>Duplicate</segment></folder>",
        1).encode()
    value = reencrypt_plaintext(copy.deepcopy(base), plaintext)
    add("duplicate-folder-path", "An index may contain only one folder path", "malformed", value)
    metadata = positives[1]["encoded"]
    plaintext = metadata["plaintext_xml"].replace(
        f'<required feature="{CONTENT_REVISION_NS}"/>', "", 1).encode()
    value = reencrypt_plaintext(copy.deepcopy(metadata), plaintext)
    add("undeclared-content-revision", "Metadata-only content revision requires a declaration", "malformed", value)
    content_case = next(case["encoded"] for case in positives if case["name"] == "content")
    plaintext = content_case["plaintext_xml"].replace(
        "<content>", f'<required feature="{CONTENT_REVISION_NS}"/><content>', 1).encode()
    value = reencrypt_plaintext(copy.deepcopy(content_case), plaintext)
    add("content-revision-on-content", "Content payloads cannot declare an index-only extension", "malformed", value)
    value = copy.deepcopy(base)
    root = ET.fromstring(value["xml"]); list(root)[0].tag = qname("encrypted", "urn:xmpp:private-notes:1"); value["xml"] = ET.tostring(root, encoding="unicode")
    add("future-major-namespace", "Unknown major namespace", "unsupported", value)
    value = reencrypt_plaintext(copy.deepcopy(base), b"not XML")
    add("malformed-plaintext", "Authenticated plaintext is not XML", "malformed", value)
    required = base["plaintext_xml"].replace("<content>", '<required feature="urn:example:required:1"/><content>', 1).encode()
    value = reencrypt_plaintext(copy.deepcopy(base), required)
    add("unknown-required-extension", "Unknown required feature", "unsupported", value)
    value = reencrypt_plaintext(copy.deepcopy(base), b'<!DOCTYPE envelope [<!ENTITY x "boom">]>' + base["plaintext_xml"].encode())
    add("doctype-plaintext", "DTD and entities are forbidden", "malformed", value)

    return {
        "vector_format": "private-notes-xml-v1",
        "description": "Portable XML/AES-GCM conformance vectors for AnyKeep PubSub records",
        "generated_by": "private-notes-encrypted-reference.py",
        "protocol_namespace": PROTOCOL_NS,
        "folder_namespace": FOLDER_NS,
        "content_revision_namespace": CONTENT_REVISION_NS,
        "crypto": {"cipher": "AES-256-GCM", "nonce_bytes": 12, "tag_bytes": 16, "aad_hex": "",
                   "hkdf_hash": "SHA-256", "hkdf_salt_utf8": HKDF_SALT.decode(),
                   "hkdf_info_prefix_utf8": HKDF_INFO_PREFIX.decode()},
        "positive": positives, "negative": negatives,
    }


def verify_vectors(document: dict[str, Any]) -> None:
    for case in document.get("positive", []):
        if decode_document(case["encoded"]) != case["expected_decoded"]:
            fail("malformed", f"positive vector {case.get('name')} decoded differently")
    for case in document.get("negative", []):
        try:
            decode_document(case["encoded"])
        except ProtocolError as error:
            if error.category != case.get("expected_error"):
                fail("malformed", f"negative vector {case.get('name')} returned {error.category}")
        else:
            fail("malformed", f"negative vector {case.get('name')} unexpectedly succeeded")


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        fail("invalid_argument", f"{path} must contain a JSON object")
    return value


def write_json(value: Any, path: Path | None) -> None:
    text = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    if path is None:
        sys.stdout.write(text)
    else:
        path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    generate = commands.add_parser("generate-vectors")
    generate.add_argument("--output", type=Path, default=Path(__file__).with_name("private-notes-encrypted-vectors.json"))
    verify = commands.add_parser("verify-vectors")
    verify.add_argument("path", nargs="?", type=Path, default=Path(__file__).with_name("private-notes-encrypted-vectors.json"))
    encode = commands.add_parser("encode"); encode.add_argument("request", type=Path); encode.add_argument("--output", type=Path)
    decode = commands.add_parser("decode"); decode.add_argument("encoded", type=Path); decode.add_argument("--master-key-hex"); decode.add_argument("--node"); decode.add_argument("--supported-feature", action="append", default=[]); decode.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "generate-vectors":
            value = generate_vectors(); verify_vectors(value); write_json(value, args.output)
        elif args.command == "verify-vectors":
            verify_vectors(load_json(args.path))
        elif args.command == "encode":
            write_json(encode_request(load_json(args.request)), args.output)
        else:
            write_json(decode_document(load_json(args.encoded), args.master_key_hex, args.node, args.supported_feature), args.output)
        return 0
    except ProtocolError as error:
        print(json.dumps({"error": error.category, "message": error.message}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
