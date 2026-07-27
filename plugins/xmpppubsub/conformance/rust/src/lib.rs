use aes_gcm::{aead::{Aead, KeyInit}, Aes256Gcm, Nonce};
use base64::{engine::general_purpose::{STANDARD, URL_SAFE_NO_PAD}, Engine as _};
use hkdf::Hkdf;
use roxmltree::{Document, Node};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

const PUBSUB_NS: &str = "http://jabber.org/protocol/pubsub";
const PROTOCOL_NS: &str = "urn:xmpp:qtnote:notes:1";
const HKDF_SALT: &[u8] = b"QtNote HKDF salt v1";
const HKDF_INFO_PREFIX: &[u8] = b"QtNote key domain v1:";
const KEY_ID_PREFIX: &[u8] = b"QtNote storage key id v1\0";

fn text<'a>(value: &'a Value, name: &str) -> Result<&'a str, String> {
    value.as_str().ok_or_else(|| format!("{name} must be text"))
}

fn bytes_hex(value: &Value, name: &str) -> Result<Vec<u8>, String> {
    hex::decode(text(value, name)?).map_err(|error| format!("invalid {name}: {error}"))
}

fn derive_key(master_key: &[u8], kind: &str) -> Result<[u8; 32], String> {
    if master_key.len() != 32 {
        return Err("master key must be 32 bytes".into());
    }
    let domain = match kind {
        "index" => b"storage-index".as_slice(),
        "content" => b"storage-content".as_slice(),
        _ => return Err("kind must be index or content".into()),
    };
    let hkdf = Hkdf::<Sha256>::new(Some(HKDF_SALT), master_key);
    let mut info = Vec::from(HKDF_INFO_PREFIX);
    info.extend_from_slice(domain);
    let mut output = [0_u8; 32];
    hkdf.expand(&info, &mut output).map_err(|_| "HKDF expansion failed".to_string())?;
    Ok(output)
}

fn storage_key_id(master_key: &[u8]) -> Result<[u8; 32], String> {
    if master_key.len() != 32 {
        return Err("master key must be 32 bytes".into());
    }
    let mut digest = Sha256::new();
    digest.update(KEY_ID_PREFIX);
    digest.update(master_key);
    Ok(digest.finalize().into())
}

fn children<'a, 'input>(node: Node<'a, 'input>, name: &str) -> Vec<Node<'a, 'input>> {
    node.children()
        .filter(|child| child.is_element()
            && child.tag_name().namespace() == Some(PROTOCOL_NS)
            && child.tag_name().name() == name)
        .collect()
}

fn simple_text(node: Node<'_, '_>, name: &str) -> Result<String, String> {
    if node.children().any(|child| child.is_element()) {
        return Err(format!("{name} must not contain child elements"));
    }
    Ok(node.children().filter(|child| child.is_text()).filter_map(|child| child.text()).collect())
}

fn direct_text_is_whitespace(node: Node<'_, '_>) -> bool {
    node.children().filter(|child| child.is_text()).all(|child| child.text().unwrap_or("").trim().is_empty())
}

fn validate_attributes(node: Node<'_, '_>, allowed_core: &[&str], context: &str, allow_foreign: bool) -> Result<(), String> {
    for attribute in node.attributes() {
        let namespace = attribute.namespace();
        if namespace.is_none() {
            if !allowed_core.contains(&attribute.name()) {
                return Err(format!("unknown core attribute in {context}"));
            }
        } else if namespace == Some(PROTOCOL_NS) || !allow_foreign {
            return Err(format!("unsupported attribute namespace in {context}"));
        }
    }
    Ok(())
}

fn validate_children(node: Node<'_, '_>, allowed_core: &[&str], context: &str) -> Result<(), String> {
    for child in node.children().filter(|child| child.is_element()) {
        match child.tag_name().namespace() {
            None => return Err(format!("unnamespaced child element in {context}")),
            Some(PROTOCOL_NS) if !allowed_core.contains(&child.tag_name().name()) => {
                return Err(format!("unknown core element in {context}"));
            }
            _ => {}
        }
    }
    Ok(())
}

fn validate_leaf(node: Node<'_, '_>, name: &str) -> Result<(), String> {
    validate_attributes(node, &[], name, false)?;
    if node.children().any(|child| child.is_element()) {
        return Err(format!("{name} must not contain child elements"));
    }
    Ok(())
}

fn canonical_base64(value: &str, name: &str) -> Result<Vec<u8>, String> {
    let compact: String = value.split_whitespace().collect();
    let decoded = STANDARD.decode(&compact).map_err(|error| format!("invalid {name} Base64: {error}"))?;
    if STANDARD.encode(&decoded) != compact {
        return Err(format!("non-canonical {name} Base64"));
    }
    Ok(decoded)
}

fn one_binary(node: Node<'_, '_>, name: &str) -> Result<Vec<u8>, String> {
    let fields = children(node, name);
    if fields.len() != 1 {
        return Err(format!("expected one simple {name} element"));
    }
    validate_leaf(fields[0], name)?;
    canonical_base64(fields[0].text().unwrap_or(""), name)
}

struct OuterPayload {
    item_id: String,
    key_id: Vec<u8>,
    nonce: Vec<u8>,
    ciphertext: Vec<u8>,
    tag: Vec<u8>,
}

fn parse_outer_xml(xml: &str) -> Result<OuterPayload, String> {
    let document = Document::parse(xml).map_err(|error| format!("invalid outer XML: {error}"))?;
    let item = document.root_element();
    let item_namespace = item.tag_name().namespace();
    if item.tag_name().name() != "item"
        || !(item_namespace.is_none() || item_namespace == Some(PUBSUB_NS))
    {
        return Err("outer XML root must be a PubSub item".into());
    }
    let item_id = item.attribute("id").filter(|value| !value.is_empty())
        .ok_or_else(|| "missing item ID".to_string())?.to_string();
    let elements: Vec<_> = item.children().filter(|node| node.is_element()).collect();
    if elements.len() != 1 || elements[0].tag_name().namespace() != Some(PROTOCOL_NS)
        || elements[0].tag_name().name() != "encrypted" {
        return Err("item must contain one current QtNote encrypted element".into());
    }
    let encrypted = elements[0];
    validate_attributes(encrypted, &["key-id"], "encrypted QtNote payload", true)?;
    validate_children(encrypted, &["nonce", "payload", "tag"], "encrypted QtNote payload")?;
    if !direct_text_is_whitespace(encrypted) {
        return Err("unexpected text in encrypted QtNote payload".into());
    }
    let key_id_text = encrypted.attribute("key-id").ok_or_else(|| "missing key-id".to_string())?;
    let key_id = URL_SAFE_NO_PAD.decode(key_id_text).map_err(|error| format!("invalid key-id: {error}"))?;
    if key_id.len() != 32 || URL_SAFE_NO_PAD.encode(&key_id) != key_id_text {
        return Err("non-canonical key-id".into());
    }
    let nonce = one_binary(encrypted, "nonce")?;
    let ciphertext = one_binary(encrypted, "payload")?;
    let tag = one_binary(encrypted, "tag")?;
    if nonce.len() != 12 || ciphertext.is_empty() || tag.len() != 16 {
        return Err("invalid AES-GCM envelope sizes".into());
    }
    Ok(OuterPayload { item_id, key_id, nonce, ciphertext, tag })
}

fn validate_plaintext(plaintext: &str, kind: &str, actual_node: &str, item_id: &str) -> Result<Value, String> {
    let upper = plaintext.to_ascii_uppercase();
    if upper.contains("<!DOCTYPE") || upper.contains("<!ENTITY") {
        return Err("DTD and entity declarations are forbidden".into());
    }
    let document = Document::parse(plaintext).map_err(|error| format!("invalid plaintext XML: {error}"))?;
    let envelope = document.root_element();
    if envelope.tag_name().namespace() != Some(PROTOCOL_NS) || envelope.tag_name().name() != "envelope" {
        return Err("plaintext root must be current QtNote envelope".into());
    }
    validate_attributes(envelope, &[], "authenticated QtNote envelope", true)?;
    validate_children(envelope, &["node", "required", "content"], "authenticated QtNote envelope")?;
    if !direct_text_is_whitespace(envelope) {
        return Err("unexpected text in authenticated QtNote envelope".into());
    }
    let required = children(envelope, "required");
    if !required.is_empty() {
        return Err("unsupported required extension".into());
    }
    let nodes = children(envelope, "node");
    let contents = children(envelope, "content");
    if nodes.len() != 1 || contents.len() != 1 {
        return Err("envelope must contain one node and one content".into());
    }
    validate_leaf(nodes[0], "node")?;
    if simple_text(nodes[0], "node")? != actual_node {
        return Err("authenticated node mismatch".into());
    }
    validate_attributes(contents[0], &[], "authenticated QtNote content", true)?;
    validate_children(contents[0], &["index", "note"], "authenticated QtNote content")?;
    if !direct_text_is_whitespace(contents[0]) {
        return Err("unexpected text in authenticated QtNote content".into());
    }
    let expected = if kind == "index" { "index" } else { "note" };
    let other = if kind == "index" { "note" } else { "index" };
    let records = children(contents[0], expected);
    if records.len() != 1 || !children(contents[0], other).is_empty() {
        return Err(format!("content must contain exactly one {expected}"));
    }
    let record = records[0];
    if record.attribute("id") != Some(item_id) {
        return Err("authenticated record ID mismatch".into());
    }
    let revision = record.attribute("revision").unwrap_or("");
    if revision.is_empty() {
        return Err("missing revision".into());
    }
    if kind == "index" {
        validate_attributes(record, &["id", "revision", "parent-revision", "origin-id", "modified", "format"],
                            "authenticated QtNote index", true)?;
        validate_children(record, &["title", "tag"], "authenticated QtNote index")?;
        if !direct_text_is_whitespace(record) {
            return Err("unexpected text in authenticated QtNote index".into());
        }
        if record.attribute("format") != Some("markdown") || record.attribute("modified").unwrap_or("").is_empty() {
            return Err("invalid index format or modified time".into());
        }
        let titles = children(record, "title");
        if titles.len() != 1 {
            return Err("index must contain one title".into());
        }
        validate_leaf(titles[0], "title")?;
        let tags: Result<Vec<_>, _> = children(record, "tag").into_iter().map(|tag| {
            validate_leaf(tag, "tag")?;
            simple_text(tag, "tag")
        }).collect();
        Ok(json!({
            "kind": kind,
            "id": item_id,
            "revision": revision,
            "parent_revision": record.attribute("parent-revision").unwrap_or(""),
            "origin_id": record.attribute("origin-id").unwrap_or(""),
            "title": simple_text(titles[0], "title")?,
            "modified": record.attribute("modified").unwrap_or(""),
            "format": record.attribute("format").unwrap_or(""),
            "tags": tags?,
        }))
    } else {
        validate_attributes(record, &["id", "revision"], "authenticated QtNote content record", true)?;
        validate_children(record, &["body"], "authenticated QtNote content record")?;
        if !direct_text_is_whitespace(record) {
            return Err("unexpected text in authenticated QtNote content record".into());
        }
        let bodies = children(record, "body");
        if bodies.len() != 1 {
            return Err("note must contain one body".into());
        }
        validate_leaf(bodies[0], "body")?;
        Ok(json!({"kind": kind, "id": item_id, "revision": revision, "body": simple_text(bodies[0], "body")?}))
    }
}

/// Verify a self-contained JSON document produced by the Python `encode` command.
pub fn verify_encoded_document(encoded: &Value) -> Result<Value, String> {
    let kind = text(&encoded["kind"], "kind")?;
    let node = text(&encoded["node"], "node")?;
    let outer = parse_outer_xml(text(&encoded["xml"], "xml")?)?;
    if outer.item_id != text(&encoded["item_id"], "item_id")? {
        return Err("outer item ID differs from JSON metadata".into());
    }
    let master_key = bytes_hex(&encoded["master_key_hex"], "master key")?;
    let key_id = storage_key_id(&master_key)?;
    if outer.key_id != key_id || hex::encode(key_id) != text(&encoded["key_id_hex"], "key ID")? {
        return Err("storage key ID mismatch".into());
    }
    let derived = derive_key(&master_key, kind)?;
    if hex::encode(derived) != text(&encoded["derived_key_hex"], "derived key")? {
        return Err("derived key mismatch".into());
    }
    let mut sealed = outer.ciphertext;
    sealed.extend_from_slice(&outer.tag);
    let cipher = Aes256Gcm::new_from_slice(&derived).map_err(|_| "invalid AES key".to_string())?;
    let plaintext = cipher.decrypt(Nonce::from_slice(&outer.nonce), sealed.as_slice())
        .map_err(|_| "AES-GCM authentication failed".to_string())?;
    if hex::encode(&plaintext) != text(&encoded["plaintext_hex"], "plaintext hex")? {
        return Err("reference plaintext bytes mismatch".into());
    }
    let plaintext_text = std::str::from_utf8(&plaintext).map_err(|error| format!("plaintext is not UTF-8: {error}"))?;
    let record = validate_plaintext(plaintext_text, kind, node, &outer.item_id)?;
    Ok(json!({
        "status": "ok",
        "protocol_namespace": PROTOCOL_NS,
        "kind": kind,
        "node": node,
        "item_id": outer.item_id,
        "key_id_hex": hex::encode(key_id),
        "plaintext_hex": hex::encode(plaintext),
        "record": record,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../../qtnote-encrypted-vectors.json");

    #[test]
    fn decrypts_python_generated_positive_vectors() {
        let document: Value = serde_json::from_str(VECTORS).expect("valid vector JSON");
        for vector in document["positive"].as_array().expect("positive vector array") {
            let name = vector["name"].as_str().expect("vector name");
            verify_encoded_document(&vector["encoded"]).unwrap_or_else(|error| panic!("{name}: {error}"));
        }
    }
}
