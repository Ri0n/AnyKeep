use aes_gcm::{aead::{Aead, KeyInit}, Aes256Gcm, Nonce};
use base64::{
    engine::general_purpose::{STANDARD, URL_SAFE_NO_PAD},
    Engine as _,
};
use hkdf::Hkdf;
use roxmltree::{Document, Node};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

const PUBSUB_NS: &str = "http://jabber.org/protocol/pubsub";
const ENCRYPTED_NS: &str = "urn:xmpp:qtnote:encrypted:1";
const STORAGE_NS: &str = "urn:xmpp:qtnote:storage:1";
const NOTE_NS: &str = "urn:xmpp:qtnote:note:1";
const HKDF_SALT: &[u8] = b"QtNote HKDF salt v1";
const HKDF_INFO_PREFIX: &[u8] = b"QtNote key domain v1:";
const KEY_ID_PREFIX: &[u8] = b"QtNote storage key id v1\0";

fn json_text<'a>(value: &'a Value, name: &str) -> Result<&'a str, String> {
    value.as_str().ok_or_else(|| format!("{name} must be text"))
}

fn json_bytes_hex(value: &Value, name: &str) -> Result<Vec<u8>, String> {
    hex::decode(json_text(value, name)?).map_err(|error| format!("invalid {name}: {error}"))
}

fn json_version(value: &Value, name: &str) -> Result<[u64; 2], String> {
    let array = value.as_array().ok_or_else(|| format!("{name} must be an array"))?;
    if array.len() != 2 {
        return Err(format!("{name} must contain major and minor"));
    }
    Ok([
        array[0].as_u64().ok_or_else(|| format!("{name} major must be uint"))?,
        array[1].as_u64().ok_or_else(|| format!("{name} minor must be uint"))?,
    ])
}

fn parse_version(text: &str, name: &str) -> Result<[u64; 2], String> {
    let mut parts = text.split('.');
    let major = parts.next().ok_or_else(|| format!("invalid {name}"))?;
    let minor = parts.next().ok_or_else(|| format!("invalid {name}"))?;
    if parts.next().is_some() || major.is_empty() || minor.is_empty() {
        return Err(format!("invalid {name}"));
    }
    Ok([
        major.parse().map_err(|_| format!("invalid {name} major"))?,
        minor.parse().map_err(|_| format!("invalid {name} minor"))?,
    ])
}

fn derive_key(master_key: &[u8], domain: &[u8]) -> Result<[u8; 32], String> {
    if master_key.len() != 32 {
        return Err("master key must be 32 bytes".into());
    }
    let hkdf = Hkdf::<Sha256>::new(Some(HKDF_SALT), master_key);
    let mut info = Vec::from(HKDF_INFO_PREFIX);
    info.extend_from_slice(domain);
    let mut output = [0_u8; 32];
    hkdf.expand(&info, &mut output)
        .map_err(|_| "HKDF expansion failed".to_string())?;
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

fn element_children<'a, 'input>(node: Node<'a, 'input>, namespace: &str, name: &str) -> Vec<Node<'a, 'input>> {
    node.children()
        .filter(|child| child.is_element()
            && child.tag_name().namespace() == Some(namespace)
            && child.tag_name().name() == name)
        .collect()
}

fn direct_text_is_whitespace(node: Node<'_, '_>) -> bool {
    node.children().filter(|child| child.is_text()).all(|child| {
        child.text().map(str::trim).unwrap_or("").is_empty()
    })
}

fn simple_text(node: Node<'_, '_>, name: &str) -> Result<String, String> {
    if node.children().any(|child| child.is_element()) {
        return Err(format!("{name} must not contain child elements"));
    }
    Ok(node.children().filter(|child| child.is_text()).filter_map(|child| child.text()).collect())
}

fn canonical_base64(text: &str, name: &str) -> Result<Vec<u8>, String> {
    let compact: String = text.split_whitespace().collect();
    let decoded = STANDARD.decode(&compact).map_err(|error| format!("invalid {name} Base64: {error}"))?;
    if STANDARD.encode(&decoded) != compact {
        return Err(format!("non-canonical {name} Base64"));
    }
    Ok(decoded)
}

fn one_binary_child(node: Node<'_, '_>, name: &str) -> Result<Vec<u8>, String> {
    let fields = element_children(node, ENCRYPTED_NS, name);
    if fields.len() != 1 || fields[0].attributes().len() != 0 || fields[0].children().any(|child| child.is_element()) {
        return Err(format!("encrypted element must contain one simple {name}"));
    }
    canonical_base64(fields[0].text().unwrap_or(""), name)
}

struct OuterPayload {
    item_id: String,
    kind: String,
    wire: [u64; 2],
    schema: [u64; 2],
    key_id: Vec<u8>,
    nonce: Vec<u8>,
    ciphertext: Vec<u8>,
    tag: Vec<u8>,
}

fn parse_outer_xml(xml: &str) -> Result<OuterPayload, String> {
    let document = Document::parse(xml).map_err(|error| format!("invalid outer XML: {error}"))?;
    let item = document.root_element();
    if item.tag_name().name() != "item"
        || !matches!(item.tag_name().namespace(), None | Some(PUBSUB_NS))
    {
        return Err("outer XML root must be a PubSub item".into());
    }
    let item_id = item.attribute("id").filter(|value| !value.is_empty())
        .ok_or_else(|| "missing item ID".to_string())?.to_string();
    let children: Vec<_> = item.children().filter(|node| node.is_element()).collect();
    if children.len() != 1 || children[0].tag_name().namespace() != Some(ENCRYPTED_NS)
        || children[0].tag_name().name() != "encrypted" {
        return Err("item must contain one QtNote encrypted element".into());
    }
    let encrypted = children[0];
    if !direct_text_is_whitespace(encrypted) {
        return Err("unexpected text in encrypted element".into());
    }
    let wire = parse_version(encrypted.attribute("wire").unwrap_or(""), "wire")?;
    let schema = parse_version(encrypted.attribute("schema").unwrap_or(""), "schema")?;
    let kind = encrypted.attribute("kind").unwrap_or("").to_string();
    if kind != "index" && kind != "content" {
        return Err("unsupported encrypted kind".into());
    }
    let key_id_text = encrypted.attribute("key-id").ok_or_else(|| "missing key-id".to_string())?;
    let key_id = URL_SAFE_NO_PAD.decode(key_id_text)
        .map_err(|error| format!("invalid key-id Base64url: {error}"))?;
    if key_id.len() != 32 || URL_SAFE_NO_PAD.encode(&key_id) != key_id_text {
        return Err("non-canonical key-id Base64url".into());
    }
    let nonce = one_binary_child(encrypted, "nonce")?;
    let ciphertext = one_binary_child(encrypted, "payload")?;
    let tag = one_binary_child(encrypted, "tag")?;
    if nonce.len() != 12 || ciphertext.is_empty() || tag.len() != 16 {
        return Err("invalid AES-GCM envelope sizes".into());
    }
    Ok(OuterPayload { item_id, kind, wire, schema, key_id, nonce, ciphertext, tag })
}

fn validate_plaintext(plaintext: &str, outer: &OuterPayload, actual_node: &str) -> Result<Value, String> {
    if plaintext.to_ascii_uppercase().contains("<!DOCTYPE") || plaintext.to_ascii_uppercase().contains("<!ENTITY") {
        return Err("DTD and entity declarations are forbidden".into());
    }
    let document = Document::parse(plaintext).map_err(|error| format!("invalid plaintext XML: {error}"))?;
    let envelope = document.root_element();
    if envelope.tag_name().namespace() != Some(STORAGE_NS) || envelope.tag_name().name() != "envelope" {
        return Err("plaintext root must be the QtNote envelope".into());
    }
    let wire = parse_version(envelope.attribute("wire").unwrap_or(""), "authenticated wire")?;
    let schema = parse_version(envelope.attribute("schema").unwrap_or(""), "authenticated schema")?;
    if wire != outer.wire || schema != outer.schema {
        return Err("outer/authenticated version mismatch".into());
    }
    let nodes = element_children(envelope, STORAGE_NS, "node");
    let contents = element_children(envelope, STORAGE_NS, "content");
    if nodes.len() != 1 || contents.len() != 1 {
        return Err("plaintext must contain one node and one content element".into());
    }
    let bound_node = simple_text(nodes[0], "node")?;
    if bound_node != actual_node {
        return Err("authenticated node mismatch".into());
    }
    let indexes = element_children(contents[0], NOTE_NS, "index");
    let notes = element_children(contents[0], NOTE_NS, "note");
    if indexes.len() + notes.len() != 1 {
        return Err("content must contain exactly one record".into());
    }
    let record = if outer.kind == "index" {
        indexes.first().copied().ok_or_else(|| "index payload does not contain index record".to_string())?
    } else {
        notes.first().copied().ok_or_else(|| "content payload does not contain note record".to_string())?
    };
    if record.attribute("id") != Some(outer.item_id.as_str()) {
        return Err("authenticated record ID mismatch".into());
    }

    let record_json = if outer.kind == "index" {
        let titles = element_children(record, NOTE_NS, "title");
        if titles.len() != 1 {
            return Err("index must contain one title".into());
        }
        let tags: Result<Vec<_>, _> = element_children(record, NOTE_NS, "tag")
            .into_iter().map(|tag| simple_text(tag, "tag")).collect();
        json!({
            "id": outer.item_id,
            "revision": record.attribute("revision").unwrap_or(""),
            "parent_revision": record.attribute("parent-revision"),
            "origin_id": record.attribute("origin-id"),
            "title": simple_text(titles[0], "title")?,
            "modified": record.attribute("modified").unwrap_or(""),
            "format": record.attribute("format").unwrap_or(""),
            "tags": tags?,
        })
    } else {
        let bodies = element_children(record, NOTE_NS, "body");
        if bodies.len() != 1 {
            return Err("content must contain one body".into());
        }
        json!({
            "id": outer.item_id,
            "revision": record.attribute("revision").unwrap_or(""),
            "body": simple_text(bodies[0], "body")?,
        })
    };

    Ok(json!({
        "kind": outer.kind,
        "node": actual_node,
        "item_id": outer.item_id,
        "wire": outer.wire,
        "schema": outer.schema,
        "record": record_json,
    }))
}

/// Verify one self-contained JSON document produced by the Python `encode` command.
///
/// The smoke validator independently checks XML framing, HKDF-SHA-256, storage
/// key ID, AES-256-GCM with empty AAD, exact plaintext bytes and the authenticated
/// PubSub node/item bindings.  Equivalent XML produced by another implementation
/// does not need to match the Python serialization byte-for-byte.
pub fn verify_encoded_document(encoded: &Value) -> Result<Value, String> {
    let xml = json_text(&encoded["xml"], "xml")?;
    let outer = parse_outer_xml(xml)?;
    let node = json_text(&encoded["node"], "node")?;
    let expected_wire = json_version(&encoded["wire"], "wire")?;
    let expected_schema = json_version(&encoded["schema"], "schema")?;
    if outer.wire != expected_wire || outer.schema != expected_schema {
        return Err("outer XML version differs from JSON metadata".into());
    }
    if outer.kind != json_text(&encoded["kind"], "kind")?
        || outer.item_id != json_text(&encoded["item_id"], "item_id")? {
        return Err("outer XML routing metadata differs from JSON metadata".into());
    }

    let master_key = json_bytes_hex(&encoded["master_key_hex"], "master key")?;
    let expected_key_id = storage_key_id(&master_key)?;
    if outer.key_id != expected_key_id {
        return Err("storage key ID mismatch".into());
    }
    if hex::encode(expected_key_id) != json_text(&encoded["key_id_hex"], "key ID hex")?
        || URL_SAFE_NO_PAD.encode(expected_key_id)
            != json_text(&encoded["key_id_base64url"], "key ID Base64url")? {
        return Err("JSON key ID representation mismatch".into());
    }

    let domain = if outer.kind == "index" { b"storage-index".as_slice() } else { b"storage-content".as_slice() };
    let derived_key = derive_key(&master_key, domain)?;
    if hex::encode(derived_key) != json_text(&encoded["derived_key_hex"], "derived key")? {
        return Err("derived key mismatch".into());
    }
    let mut sealed = Vec::with_capacity(outer.ciphertext.len() + outer.tag.len());
    sealed.extend_from_slice(&outer.ciphertext);
    sealed.extend_from_slice(&outer.tag);
    let cipher = Aes256Gcm::new_from_slice(&derived_key).map_err(|_| "invalid AES key".to_string())?;
    let plaintext = cipher.decrypt(Nonce::from_slice(&outer.nonce), sealed.as_slice())
        .map_err(|_| "AES-GCM authentication failed".to_string())?;
    if hex::encode(&plaintext) != json_text(&encoded["plaintext_hex"], "plaintext hex")? {
        return Err("exact reference plaintext bytes mismatch".into());
    }
    let plaintext_text = std::str::from_utf8(&plaintext).map_err(|error| format!("plaintext is not UTF-8: {error}"))?;
    let mut result = validate_plaintext(plaintext_text, &outer, node)?;
    result["status"] = json!("ok");
    result["key_id_hex"] = json!(hex::encode(expected_key_id));
    result["plaintext_hex"] = json!(hex::encode(plaintext));
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    const VECTORS: &str = include_str!("../../../qtnote-encrypted-vectors.json");

    #[test]
    fn decrypts_python_generated_positive_vectors() {
        let document: Value = serde_json::from_str(VECTORS).expect("valid vector JSON");
        let positives = document["positive"].as_array().expect("positive vector array");
        assert!(!positives.is_empty());
        for vector in positives {
            let name = vector["name"].as_str().expect("vector name");
            verify_encoded_document(&vector["encoded"])
                .unwrap_or_else(|error| panic!("{name}: {error}"));
        }
    }
}
