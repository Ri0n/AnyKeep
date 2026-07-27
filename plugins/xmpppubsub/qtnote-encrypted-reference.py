#!/usr/bin/env python3
"""Independent QtNote encrypted PubSub XML reference codec.

This file is intentionally free of Qt dependencies.  It is both a small CLI and
an executable companion to PROTOXEP.md.  The production protocol encrypts UTF-8
XML with AES-256-GCM; XML serialization is not canonical and interoperability is
validated semantically after decryption.
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
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

PUBSUB_NS = "http://jabber.org/protocol/pubsub"
ENCRYPTED_NS = "urn:xmpp:qtnote:encrypted:1"
STORAGE_NS = "urn:xmpp:qtnote:storage:1"
NOTE_NS = "urn:xmpp:qtnote:note:1"
WIRE = (1, 0)
SCHEMA = (1, 0)
MAX_XML_SIZE = 16 * 1024 * 1024
HKDF_SALT = b"QtNote HKDF salt v1"
HKDF_INFO_PREFIX = b"QtNote key domain v1:"
KEY_ID_PREFIX = b"QtNote storage key id v1\0"

ET.register_namespace("", STORAGE_NS)
ET.register_namespace("note", NOTE_NS)
ET.register_namespace("enc", ENCRYPTED_NS)


class ProtocolError(Exception):
    """An expected conformance failure with a stable category."""

    def __init__(self, category: str, message: str):
        super().__init__(message)
        self.category = category
        self.message = message


@dataclass(frozen=True)
class OuterPayload:
    item_id: str
    kind: str
    wire: tuple[int, int]
    schema: tuple[int, int]
    key_id: bytes
    nonce: bytes
    ciphertext: bytes
    tag: bytes


def fail(category: str, message: str) -> None:
    raise ProtocolError(category, message)


def qname(namespace: str, local: str) -> str:
    return f"{{{namespace}}}{local}"


def split_qname(name: str) -> tuple[str, str]:
    if name.startswith("{"):
        namespace, local = name[1:].split("}", 1)
        return namespace, local
    return "", name


def parse_version(value: Any, name: str) -> tuple[int, int]:
    if isinstance(value, str):
        match = re.fullmatch(r"([0-9]+)\.([0-9]+)", value)
        if not match:
            fail("malformed", f"{name} must be major.minor")
        major, minor = int(match.group(1)), int(match.group(2))
    elif isinstance(value, list) and len(value) == 2 and all(isinstance(part, int) for part in value):
        major, minor = value
    else:
        fail("invalid_argument", f"{name} must be [major, minor] or major.minor")
    if not (0 <= major <= 65535 and 0 <= minor <= 65535):
        fail("malformed", f"{name} is outside the uint16 range")
    return major, minor


def version_text(version: tuple[int, int]) -> str:
    return f"{version[0]}.{version[1]}"


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


def derive_key(master_key: bytes, domain: str) -> bytes:
    """RFC 5869 HKDF-SHA-256, L=32.

    For one SHA-256 block, HKDF-Expand is HMAC(PRK, info || 0x01).  The block
    counter is not part of the public info parameter.
    """

    if len(master_key) != 32:
        fail("invalid_argument", "master key must contain 32 bytes")
    if domain not in ("storage-index", "storage-content"):
        fail("invalid_argument", f"unknown key domain {domain}")
    prk = hmac.new(HKDF_SALT, master_key, hashlib.sha256).digest()
    info = HKDF_INFO_PREFIX + domain.encode("ascii")
    return hmac.new(prk, info + b"\x01", hashlib.sha256).digest()


def b64(value: bytes) -> str:
    return base64.b64encode(value).decode("ascii")


def b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def decode_canonical_base64(text: str, name: str, maximum_size: int) -> bytes:
    compact = "".join(text.split())
    if not compact or len(compact) % 4 == 1 or len(compact) > ((maximum_size + 2) // 3) * 4:
        fail("malformed", f"invalid {name} Base64 length")
    try:
        decoded = base64.b64decode(compact, validate=True)
    except Exception as error:
        fail("malformed", f"invalid {name} Base64: {error}")
    if len(decoded) > maximum_size or b64(decoded) != compact:
        fail("malformed", f"non-canonical {name} Base64")
    return decoded


def decode_canonical_base64url(text: str, name: str, expected_size: int) -> bytes:
    if not isinstance(text, str) or "=" in text or not re.fullmatch(r"[A-Za-z0-9_-]+", text or ""):
        fail("malformed", f"invalid {name} Base64url")
    padded = text + "=" * ((4 - len(text) % 4) % 4)
    try:
        decoded = base64.urlsafe_b64decode(padded)
    except Exception as error:
        fail("malformed", f"invalid {name} Base64url: {error}")
    if len(decoded) != expected_size or b64url(decoded) != text:
        fail("malformed", f"non-canonical {name} Base64url")
    return decoded


def reject_unsafe_xml(data: bytes, name: str) -> None:
    if not data or len(data) > MAX_XML_SIZE:
        fail("malformed" if not data else "unsupported", f"invalid {name} size")
    upper = data.upper()
    if b"<!DOCTYPE" in upper or b"<!ENTITY" in upper:
        fail("malformed", f"document types and entity declarations are forbidden in {name}")


def parse_xml(data: bytes, name: str) -> ET.Element:
    reject_unsafe_xml(data, name)
    try:
        return ET.fromstring(data)
    except ET.ParseError as error:
        fail("malformed", f"invalid {name} XML: {error}")


def direct_children(parent: ET.Element, namespace: str, local: str) -> list[ET.Element]:
    return [child for child in list(parent) if child.tag == qname(namespace, local)]


def simple_text(element: ET.Element, name: str) -> str:
    if list(element):
        fail("malformed", f"{name} must not contain child elements")
    return element.text or ""


def direct_text_is_whitespace(element: ET.Element) -> bool:
    if element.text and element.text.strip():
        return False
    return all(not child.tail or not child.tail.strip() for child in list(element))


def parse_outer_xml(xml_bytes: bytes) -> OuterPayload:
    root = parse_xml(xml_bytes, "PubSub item")
    if root.tag not in ("item", qname(PUBSUB_NS, "item")):
        fail("malformed", "outer XML root must be a PubSub item element")
    item_id = root.attrib.get("id", "")
    if not item_id:
        fail("malformed", "PubSub item ID is missing")
    children = list(root)
    if len(children) != 1 or children[0].tag != qname(ENCRYPTED_NS, "encrypted"):
        fail("malformed", "PubSub item must contain exactly one QtNote encrypted element")
    encrypted = children[0]
    if not direct_text_is_whitespace(encrypted):
        # This is the development pre-XML shape: direct Base64 text.
        if not list(encrypted):
            fail("obsolete", "obsolete pre-XML QtNote payload")
        fail("malformed", "unexpected text in encrypted QtNote element")

    wire = parse_version(encrypted.attrib.get("wire", ""), "wire")
    schema = parse_version(encrypted.attrib.get("schema", ""), "schema")
    if wire[0] != WIRE[0] or schema[0] != SCHEMA[0]:
        fail("unsupported", "unsupported encrypted QtNote major version")
    kind = encrypted.attrib.get("kind", "")
    if kind not in ("index", "content"):
        fail("unsupported", "unsupported encrypted QtNote kind")
    key_id = decode_canonical_base64url(encrypted.attrib.get("key-id", ""), "key ID", 32)

    def one_field(name: str, maximum: int) -> bytes:
        fields = direct_children(encrypted, ENCRYPTED_NS, name)
        if len(fields) != 1 or fields[0].attrib or list(fields[0]):
            fail("malformed", f"encrypted QtNote payload must contain one simple {name} element")
        return decode_canonical_base64(fields[0].text or "", name, maximum)

    nonce = one_field("nonce", 12)
    ciphertext = one_field("payload", MAX_XML_SIZE)
    tag = one_field("tag", 16)
    if len(nonce) != 12 or not ciphertext or len(tag) != 16:
        fail("malformed", "invalid AES-GCM envelope field sizes")
    return OuterPayload(item_id, kind, wire, schema, key_id, nonce, ciphertext, tag)


def required_features(root: ET.Element) -> list[str]:
    result: list[str] = []
    for element in direct_children(root, STORAGE_NS, "required"):
        if set(element.attrib) != {"feature"} or list(element) or (element.text and element.text.strip()):
            fail("malformed", "invalid required-extension declaration")
        feature = element.attrib["feature"]
        if not feature or feature in result:
            fail("malformed", "invalid or duplicate required extension")
        result.append(feature)
    return result


def validate_plaintext(
    plaintext: bytes,
    outer: OuterPayload,
    actual_node: str,
    supported_features: Iterable[str] = (),
) -> dict[str, Any]:
    root = parse_xml(plaintext, "authenticated plaintext")
    if root.tag != qname(STORAGE_NS, "envelope"):
        fail("malformed", "authenticated plaintext root must be the QtNote envelope")
    inner_wire = parse_version(root.attrib.get("wire", ""), "authenticated wire")
    inner_schema = parse_version(root.attrib.get("schema", ""), "authenticated schema")
    if inner_wire[0] != WIRE[0] or inner_schema[0] != SCHEMA[0]:
        fail("unsupported", "unsupported authenticated QtNote major version")
    if inner_wire != outer.wire or inner_schema != outer.schema:
        fail("authentication_failed", "outer and authenticated versions differ")
    if not direct_text_is_whitespace(root):
        fail("malformed", "unexpected text in authenticated QtNote envelope")

    nodes = direct_children(root, STORAGE_NS, "node")
    contents = direct_children(root, STORAGE_NS, "content")
    if len(nodes) != 1 or len(contents) != 1:
        fail("malformed", "authenticated envelope must contain one node and one content element")
    bound_node = simple_text(nodes[0], "node")
    if bound_node != actual_node:
        fail("context_mismatch", "authenticated PubSub node does not match the actual node")

    unknown_required = sorted(set(required_features(root)) - set(supported_features))
    if unknown_required:
        fail("unsupported", "unsupported required extensions: " + ", ".join(unknown_required))

    content = contents[0]
    if not direct_text_is_whitespace(content):
        fail("malformed", "unexpected text in content container")
    indexes = direct_children(content, NOTE_NS, "index")
    notes = direct_children(content, NOTE_NS, "note")
    if len(indexes) + len(notes) != 1:
        fail("malformed", "content must contain exactly one index or note record")
    if outer.kind == "index" and len(indexes) != 1:
        fail("malformed", "index payload does not contain an index record")
    if outer.kind == "content" and len(notes) != 1:
        fail("malformed", "content payload does not contain a note record")
    record = indexes[0] if outer.kind == "index" else notes[0]
    if record.attrib.get("id", "") != outer.item_id:
        fail("context_mismatch", "authenticated record ID does not match PubSub item ID")

    if outer.kind == "index":
        revision = record.attrib.get("revision", "")
        modified = record.attrib.get("modified", "")
        format_name = record.attrib.get("format", "")
        if not revision or not modified:
            fail("malformed", "index revision or modified time is missing")
        if format_name != "markdown":
            fail("unsupported", f"unsupported note format {format_name}")
        if not modified.endswith("Z"):
            fail("malformed", "modified time must be UTC")
        try:
            datetime.fromisoformat(modified[:-1] + "+00:00")
        except ValueError:
            fail("malformed", "invalid modified time")
        titles = direct_children(record, NOTE_NS, "title")
        if len(titles) != 1:
            fail("malformed", "index must contain one title")
        result_record: dict[str, Any] = {
            "id": outer.item_id,
            "revision": revision,
            "title": simple_text(titles[0], "title"),
            "modified": modified,
            "format": format_name,
            "tags": [simple_text(tag, "tag") for tag in direct_children(record, NOTE_NS, "tag")],
        }
        if "parent-revision" in record.attrib:
            result_record["parent_revision"] = record.attrib["parent-revision"]
        if "origin-id" in record.attrib:
            result_record["origin_id"] = record.attrib["origin-id"]
    else:
        revision = record.attrib.get("revision", "")
        if not revision:
            fail("malformed", "content revision is missing")
        bodies = direct_children(record, NOTE_NS, "body")
        if len(bodies) != 1:
            fail("malformed", "content must contain one body")
        result_record = {
            "id": outer.item_id,
            "revision": revision,
            "body": simple_text(bodies[0], "body"),
        }

    return {
        "kind": outer.kind,
        "node": actual_node,
        "item_id": outer.item_id,
        "wire": list(inner_wire),
        "schema": list(inner_schema),
        "required_features": required_features(root),
        "record": result_record,
        "plaintext_xml": plaintext.decode("utf-8"),
        "plaintext_hex": plaintext.hex(),
    }


def append_xml_fragments(parent: ET.Element, fragments: Any, name: str) -> None:
    if fragments is None:
        return
    if not isinstance(fragments, list) or not all(isinstance(value, str) for value in fragments):
        fail("invalid_argument", f"{name} must be an array of XML strings")
    for fragment in fragments:
        element = parse_xml(fragment.encode("utf-8"), name)
        parent.append(element)


def build_plaintext(request: dict[str, Any], kind: str, item_id: str, node: str,
                    wire: tuple[int, int], schema: tuple[int, int]) -> bytes:
    supplied = request.get("plaintext_xml")
    if supplied is not None:
        if not isinstance(supplied, str):
            fail("invalid_argument", "plaintext_xml must be text")
        return supplied.encode("utf-8")

    record_data = request.get("record")
    if not isinstance(record_data, dict):
        fail("invalid_argument", "record must be an object")
    root = ET.Element(qname(STORAGE_NS, "envelope"), {
        "wire": version_text(wire),
        "schema": version_text(schema),
    })
    node_element = ET.SubElement(root, qname(STORAGE_NS, "node"))
    node_element.text = node
    features = request.get("required_features", [])
    if not isinstance(features, list) or not all(isinstance(value, str) and value for value in features):
        fail("invalid_argument", "required_features must be an array of non-empty strings")
    for feature in features:
        ET.SubElement(root, qname(STORAGE_NS, "required"), {"feature": feature})
    append_xml_fragments(root, request.get("envelope_extensions_xml"), "envelope extension")
    content = ET.SubElement(root, qname(STORAGE_NS, "content"))

    if kind == "index":
        revision = record_data.get("revision")
        modified = record_data.get("modified")
        if not isinstance(revision, str) or not revision or not isinstance(modified, str) or not modified:
            fail("invalid_argument", "index revision and modified are required")
        attributes = {
            "id": item_id,
            "revision": revision,
            "modified": modified,
            "format": record_data.get("format", "markdown"),
        }
        if record_data.get("parent_revision"):
            attributes["parent-revision"] = record_data["parent_revision"]
        if record_data.get("origin_id"):
            attributes["origin-id"] = record_data["origin_id"]
        record = ET.SubElement(content, qname(NOTE_NS, "index"), attributes)
        title = ET.SubElement(record, qname(NOTE_NS, "title"))
        title.text = str(record_data.get("title", ""))
        tags = record_data.get("tags", [])
        if not isinstance(tags, list) or not all(isinstance(tag, str) for tag in tags):
            fail("invalid_argument", "tags must be an array of strings")
        for value in tags:
            tag = ET.SubElement(record, qname(NOTE_NS, "tag"))
            tag.text = value
    else:
        revision = record_data.get("revision")
        if not isinstance(revision, str) or not revision:
            fail("invalid_argument", "content revision is required")
        record = ET.SubElement(content, qname(NOTE_NS, "note"), {
            "id": item_id,
            "revision": revision,
        })
        body = ET.SubElement(record, qname(NOTE_NS, "body"))
        body.text = str(record_data.get("body", ""))
    append_xml_fragments(record, record_data.get("extensions_xml"), "record extension")
    return ET.tostring(root, encoding="utf-8", short_empty_elements=True)


def outer_xml(payload: OuterPayload) -> str:
    item = ET.Element("item", {"id": payload.item_id})
    encrypted = ET.SubElement(item, "encrypted", {
        "xmlns": ENCRYPTED_NS,
        "wire": version_text(payload.wire),
        "schema": version_text(payload.schema),
        "kind": payload.kind,
        "key-id": b64url(payload.key_id),
    })
    ET.SubElement(encrypted, "nonce").text = b64(payload.nonce)
    ET.SubElement(encrypted, "payload").text = b64(payload.ciphertext)
    ET.SubElement(encrypted, "tag").text = b64(payload.tag)
    return ET.tostring(item, encoding="unicode", short_empty_elements=True)


def encode_request(request: dict[str, Any]) -> dict[str, Any]:
    kind = request.get("kind")
    if kind not in ("index", "content"):
        fail("invalid_argument", "kind must be index or content")
    item_id = request.get("item_id")
    node = request.get("node")
    if not isinstance(item_id, str) or not item_id or not isinstance(node, str) or not node:
        fail("invalid_argument", "item_id and node are required")
    wire = parse_version(request.get("wire", list(WIRE)), "wire")
    schema = parse_version(request.get("schema", list(SCHEMA)), "schema")
    if wire[0] != WIRE[0] or schema[0] != SCHEMA[0]:
        fail("unsupported", "reference encoder only writes the supported major version")
    master_key = hex_bytes(request.get("master_key_hex"), "master key", 32)
    nonce_value = request.get("nonce_hex")
    nonce = os.urandom(12) if nonce_value is None else hex_bytes(nonce_value, "nonce", 12)
    plaintext = build_plaintext(request, kind, item_id, node, wire, schema)
    if len(plaintext) > MAX_XML_SIZE:
        fail("invalid_argument", "plaintext XML exceeds the implementation limit")
    domain = "storage-index" if kind == "index" else "storage-content"
    derived_key = derive_key(master_key, domain)
    sealed = AESGCM(derived_key).encrypt(nonce, plaintext, None)
    ciphertext, tag = sealed[:-16], sealed[-16:]
    payload = OuterPayload(item_id, kind, wire, schema, storage_key_id(master_key), nonce, ciphertext, tag)
    encoded = {
        "kind": kind,
        "node": node,
        "item_id": item_id,
        "wire": list(wire),
        "schema": list(schema),
        "master_key_hex": master_key.hex(),
        "derived_key_hex": derived_key.hex(),
        "key_id_hex": payload.key_id.hex(),
        "key_id_base64url": b64url(payload.key_id),
        "nonce_hex": nonce.hex(),
        "nonce_base64": b64(nonce),
        "ciphertext_hex": ciphertext.hex(),
        "ciphertext_base64": b64(ciphertext),
        "tag_hex": tag.hex(),
        "tag_base64": b64(tag),
        "plaintext_xml": plaintext.decode("utf-8"),
        "plaintext_hex": plaintext.hex(),
        "xml": outer_xml(payload),
    }
    # Verify the encoder's own output semantically before returning it.
    decode_document(encoded)
    return encoded


def decode_document(encoded: dict[str, Any], *, master_key_override: str | None = None,
                    node_override: str | None = None, supported_features: Iterable[str] = ()) -> dict[str, Any]:
    xml_text = encoded.get("xml")
    if not isinstance(xml_text, str):
        fail("invalid_argument", "encoded document must contain xml text")
    outer = parse_outer_xml(xml_text.encode("utf-8"))
    node = node_override if node_override is not None else encoded.get("node")
    if not isinstance(node, str) or not node:
        fail("invalid_argument", "actual PubSub node is required")
    key_hex = master_key_override if master_key_override is not None else encoded.get("master_key_hex")
    master_key = hex_bytes(key_hex, "master key", 32)
    expected_key_id = storage_key_id(master_key)
    if outer.key_id != expected_key_id:
        fail("wrong_key", "encrypted item was written with another storage key")
    domain = "storage-index" if outer.kind == "index" else "storage-content"
    derived_key = derive_key(master_key, domain)
    try:
        plaintext = AESGCM(derived_key).decrypt(outer.nonce, outer.ciphertext + outer.tag, None)
    except Exception:
        fail("authentication_failed", "AES-GCM authentication failed")
    result = validate_plaintext(plaintext, outer, node, supported_features)
    result.update({
        "key_id_hex": outer.key_id.hex(),
        "key_id_base64url": b64url(outer.key_id),
        "derived_key_hex": derived_key.hex(),
        "nonce_hex": outer.nonce.hex(),
        "ciphertext_hex": outer.ciphertext.hex(),
        "tag_hex": outer.tag.hex(),
    })
    return result


def namespace_pubsub_item(xml: str) -> str:
    if not xml.startswith("<item "):
        fail("invalid_argument", "reference item XML has an unexpected wrapper")
    return xml.replace("<item ", f'<item xmlns="{PUBSUB_NS}" ', 1)


def replace_outer_field(xml: str, name: str, value: str) -> str:
    root = ET.fromstring(xml)
    encrypted = list(root)[0]
    encrypted.set(name, value)
    return ET.tostring(root, encoding="unicode", short_empty_elements=True)


def replace_item_id(xml: str, item_id: str) -> str:
    root = ET.fromstring(xml)
    root.set("id", item_id)
    return ET.tostring(root, encoding="unicode", short_empty_elements=True)


def replace_binary_field(xml: str, name: str, value: bytes, canonical: bool = True) -> str:
    root = ET.fromstring(xml)
    encrypted = list(root)[0]
    field = direct_children(encrypted, ENCRYPTED_NS, name)[0]
    text = b64(value)
    field.text = text if canonical else text.rstrip("=")
    return ET.tostring(root, encoding="unicode", short_empty_elements=True)


def reencrypt_plaintext(encoded: dict[str, Any], plaintext: bytes) -> dict[str, Any]:
    result = copy.deepcopy(encoded)
    master_key = bytes.fromhex(result["master_key_hex"])
    domain = "storage-index" if result["kind"] == "index" else "storage-content"
    nonce = bytes.fromhex(result["nonce_hex"])
    sealed = AESGCM(derive_key(master_key, domain)).encrypt(nonce, plaintext, None)
    ciphertext, tag = sealed[:-16], sealed[-16:]
    result["plaintext_xml"] = plaintext.decode("utf-8", errors="replace")
    result["plaintext_hex"] = plaintext.hex()
    result["ciphertext_hex"] = ciphertext.hex()
    result["ciphertext_base64"] = b64(ciphertext)
    result["tag_hex"] = tag.hex()
    result["tag_base64"] = b64(tag)
    result["xml"] = replace_binary_field(result["xml"], "payload", ciphertext)
    result["xml"] = replace_binary_field(result["xml"], "tag", tag)
    return result


def reference_requests() -> list[tuple[str, str, dict[str, Any]]]:
    key = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    item = "2b7e1516-28ae-4d2a-abf7-158809cf4f3c"
    revision = "018f0be0-df0d-7c70-a2ef-1f5c973ec92a"
    return [
        ("index-basic", "Portable index record", {
            "kind": "index", "master_key_hex": key,
            "node": "urn:xmpp:qtnote:notes:0:index:1", "item_id": item,
            "nonce_hex": "000102030405060708090a0b",
            "record": {"revision": revision, "origin_id": "device-a", "title": "Portable note",
                       "modified": "2026-07-27T18:00:00.123Z", "format": "markdown", "tags": ["one", "two"]},
        }),
        ("content-basic", "Portable content record", {
            "kind": "content", "master_key_hex": key,
            "node": "urn:xmpp:qtnote:notes:0:content:1", "item_id": item,
            "nonce_hex": "0c0d0e0f1011121314151617",
            "record": {"revision": revision, "body": "Portable body\n"},
        }),
        ("index-minor-extensions", "Higher minor, Unicode and optional XML extensions", {
            "kind": "index", "master_key_hex": key,
            "node": "urn:xmpp:qtnote:notes:0:index:1", "item_id": "unicode-note",
            "wire": [1, 2], "schema": [1, 4], "nonce_hex": "18191a1b1c1d1e1f20212223",
            "plaintext_xml": (
                '<envelope xmlns="urn:xmpp:qtnote:storage:1" xmlns:n="urn:xmpp:qtnote:note:1" '
                'xmlns:x="urn:example:qtnote:extension:1" wire="1.2" schema="1.4" x:root="preserve">'
                '<node>urn:xmpp:qtnote:notes:0:index:1</node><x:envelope value="42"/><content x:box="yes">'
                '<n:index id="unicode-note" revision="revision-unicode" modified="2026-07-27T18:00:00.000Z" '
                'format="markdown" x:record="keep"><n:title>Привет &amp; café</n:title><n:tag>тест</n:tag>'
                '<x:record>future</x:record></n:index></content></envelope>'
            ),
        }),
        ("index-minimal", "Empty title and no optional index fields", {
            "kind": "index", "master_key_hex": key,
            "node": "urn:xmpp:qtnote:notes:0:index:1", "item_id": "minimal-note",
            "nonce_hex": "2425262728292a2b2c2d2e2f",
            "record": {"revision": "minimal-revision", "title": "", "modified": "2026-07-27T18:00:00Z",
                       "format": "markdown", "tags": []},
        }),
        ("content-empty", "Empty note body", {
            "kind": "content", "master_key_hex": key,
            "node": "urn:xmpp:qtnote:notes:0:content:1", "item_id": "empty-note",
            "nonce_hex": "303132333435363738393a3b",
            "record": {"revision": "empty-revision", "body": ""},
        }),
    ]


def generate_vectors() -> dict[str, Any]:
    positives: list[dict[str, Any]] = []
    for name, description, request in reference_requests():
        encoded = encode_request(request)
        if name == "index-minimal":
            encoded["xml"] = namespace_pubsub_item(encoded["xml"])
            decode_document(encoded)
        positives.append({
            "name": name,
            "description": description,
            "request": request,
            "encoded": encoded,
            "expected_decoded": decode_document(encoded),
        })

    base = positives[0]["encoded"]
    negatives: list[dict[str, Any]] = []

    def add(name: str, description: str, category: str, encoded: dict[str, Any]) -> None:
        negatives.append({"name": name, "description": description,
                          "expected_error": category, "encoded": encoded})

    value = copy.deepcopy(base)
    root = ET.fromstring(value["xml"])
    encrypted = list(root)[0]
    for child in list(encrypted):
        encrypted.remove(child)
    encrypted.text = b64(b"obsolete-development-envelope")
    value["xml"] = ET.tostring(root, encoding="unicode")
    add("obsolete-pre-xml", "Direct-text development envelope predating the XML wire format", "obsolete", value)

    value = copy.deepcopy(base)
    value["master_key_hex"] = "55" * 32
    add("wrong-key", "Configured master key does not match key-id", "wrong_key", value)

    value = copy.deepcopy(base)
    value["node"] = "urn:xmpp:qtnote:notes:0:another-index:1"
    add("wrong-node", "Authenticated node binding differs from actual PubSub node", "context_mismatch", value)

    value = copy.deepcopy(base)
    value["item_id"] = "moved-item"
    value["xml"] = replace_item_id(value["xml"], "moved-item")
    add("wrong-item-id", "Authenticated record ID differs from outer item ID", "context_mismatch", value)

    value = copy.deepcopy(base)
    changed_tag = bytearray.fromhex(value["tag_hex"])
    changed_tag[0] ^= 1
    value["xml"] = replace_binary_field(value["xml"], "tag", bytes(changed_tag))
    add("tampered-tag", "AES-GCM tag was modified", "authentication_failed", value)

    value = copy.deepcopy(base)
    root = ET.fromstring(value["xml"])
    encrypted = list(root)[0]
    encrypted.remove(direct_children(encrypted, ENCRYPTED_NS, "tag")[0])
    value["xml"] = ET.tostring(root, encoding="unicode")
    add("missing-tag", "Current outer envelope is structurally incomplete", "malformed", value)

    value = copy.deepcopy(base)
    ciphertext = bytes.fromhex(value["ciphertext_hex"])
    value["xml"] = replace_binary_field(value["xml"], "payload", ciphertext, canonical=False)
    add("noncanonical-base64", "Padded Base64 field is not canonical", "malformed", value)

    value = copy.deepcopy(base)
    value["wire"] = [2, 0]
    value["xml"] = replace_outer_field(value["xml"], "wire", "2.0")
    add("future-wire-major", "Unknown wire major must be protected", "unsupported", value)

    value = copy.deepcopy(base)
    value["schema"] = [2, 0]
    value["xml"] = replace_outer_field(value["xml"], "schema", "2.0")
    add("future-schema-major", "Unknown schema major must be protected", "unsupported", value)

    value = copy.deepcopy(base)
    value["wire"] = [1, 9]
    value["xml"] = replace_outer_field(value["xml"], "wire", "1.9")
    add("outer-version-tampering", "Outer minor differs from authenticated version", "authentication_failed", value)

    value = reencrypt_plaintext(copy.deepcopy(base), b"not XML")
    add("malformed-plaintext", "Authenticated plaintext is not XML", "malformed", value)

    required_plaintext = base["plaintext_xml"].replace(
        "<content>", '<required feature="urn:example:required:1"/><content>', 1).encode("utf-8")
    value = reencrypt_plaintext(copy.deepcopy(base), required_plaintext)
    add("unknown-required-extension", "Unknown required feature blocks interpretation", "unsupported", value)

    dtd_plaintext = (b'<!DOCTYPE envelope [<!ENTITY x "boom">]>' + base["plaintext_xml"].encode("utf-8"))
    value = reencrypt_plaintext(copy.deepcopy(base), dtd_plaintext)
    add("doctype-plaintext", "DTD and entity declarations are forbidden", "malformed", value)

    return {
        "vector_format": "qtnote-encrypted-xml-v1",
        "description": "Portable XML/AES-GCM conformance vectors for QtNote PubSub records",
        "generated_by": "qtnote-encrypted-reference.py",
        "crypto": {
            "key_id": "SHA-256(UTF-8('QtNote storage key id v1') || 00 || master_key)",
            "hkdf_hash": "SHA-256",
            "hkdf_salt_utf8": HKDF_SALT.decode("ascii"),
            "hkdf_info_prefix_utf8": HKDF_INFO_PREFIX.decode("ascii"),
            "index_domain": "storage-index",
            "content_domain": "storage-content",
            "cipher": "AES-256-GCM",
            "nonce_bytes": 12,
            "tag_bytes": 16,
            "aad_hex": "",
        },
        "positive": positives,
        "negative": negatives,
    }


def verify_vectors(document: dict[str, Any]) -> None:
    positives = document.get("positive")
    negatives = document.get("negative")
    if not isinstance(positives, list) or not isinstance(negatives, list):
        fail("invalid_argument", "vector file must contain positive and negative arrays")
    for case in positives:
        actual = decode_document(case["encoded"])
        if actual != case["expected_decoded"]:
            fail("malformed", f"positive vector {case.get('name')} decoded differently")
    for case in negatives:
        expected = case.get("expected_error")
        try:
            decode_document(case["encoded"])
        except ProtocolError as error:
            if error.category != expected:
                fail("malformed", f"negative vector {case.get('name')} returned {error.category}, expected {expected}")
        else:
            fail("malformed", f"negative vector {case.get('name')} unexpectedly succeeded")


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        fail("invalid_argument", f"{path} must contain a JSON object")
    return value


def write_json(value: Any, path: Path | None) -> None:
    text = json.dumps(value, ensure_ascii=False, indent=2, sort_keys=False) + "\n"
    if path is None:
        sys.stdout.write(text)
    else:
        path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("generate-vectors", help="regenerate the checked-in conformance vectors")
    generate.add_argument("--output", type=Path,
                          default=Path(__file__).with_name("qtnote-encrypted-vectors.json"))

    verify = subparsers.add_parser("verify-vectors", help="verify all positive and negative vectors")
    verify.add_argument("path", nargs="?", type=Path,
                        default=Path(__file__).with_name("qtnote-encrypted-vectors.json"))

    encode = subparsers.add_parser("encode", help="encrypt a JSON request and emit a self-contained JSON document")
    encode.add_argument("request", type=Path)
    encode.add_argument("--output", type=Path)

    decode = subparsers.add_parser("decode", help="decrypt and validate a self-contained JSON document")
    decode.add_argument("encoded", type=Path)
    decode.add_argument("--master-key-hex")
    decode.add_argument("--node")
    decode.add_argument("--supported-feature", action="append", default=[])
    decode.add_argument("--output", type=Path)

    args = parser.parse_args()
    try:
        if args.command == "generate-vectors":
            vectors = generate_vectors()
            verify_vectors(vectors)
            write_json(vectors, args.output)
            print(f"wrote {args.output}", file=sys.stderr)
        elif args.command == "verify-vectors":
            verify_vectors(load_json(args.path))
            print(f"verified {args.path}")
        elif args.command == "encode":
            write_json(encode_request(load_json(args.request)), args.output)
        elif args.command == "decode":
            encoded = load_json(args.encoded)
            result = decode_document(encoded, master_key_override=args.master_key_hex,
                                     node_override=args.node, supported_features=args.supported_feature)
            write_json(result, args.output)
        return 0
    except ProtocolError as error:
        print(json.dumps({"error": error.category, "message": error.message}, ensure_ascii=False), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
