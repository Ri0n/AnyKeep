# Private Notes encrypted PubSub XML interoperability guide

This guide is the operational companion to `PROTOXEP.md`. It shows how an
independent implementation can reproduce the key derivation and encryption,
exchange arbitrary records with the Python reference tool, and validate the
result without Qt or Private Notes production code.

## Files

| File | Purpose |
| --- | --- |
| `PROTOXEP.md` | Normative protocol description |
| `private-notes.xsd` | Reference schema for the major-version-1 core XML |
| `private-notes-encrypted-reference.py` | Independent Python encoder, decoder, and vector generator |
| `private-notes-encrypted-vectors.json` | Fixed positive and negative conformance cases |
| `conformance/rust/` | Independent Rust decryption and XML validation smoke test |

The XSD describes only core elements in `urn:xmpp:private-notes:0`. Compatible
optional extensions use their own namespaces and therefore may not be fully
validated by the core schema. Schema validation does not replace the runtime
checks for node binding, item binding, required extensions, resource limits,
or cryptographic authentication.

## Version model

There is one on-wire protocol version:

```text
urn:xmpp:private-notes:0
```

The final `0` identifies the initial experimental protocol version. It is used as:

- the default PubSub base node;
- the namespace of the outer `encrypted` element;
- the namespace of the authenticated plaintext XML.

The default leaf nodes are:

```text
urn:xmpp:private-notes:0:index
urn:xmpp:private-notes:0:content
```

There are no `wire`, `schema`, `kind`, or minor-version fields in the XML.
The actual PubSub node selects index versus content and therefore also selects
the HKDF domain. Compatible changes are represented by optional XML in a
separate namespace. An incompatible change uses a new namespace and new nodes,
for example `urn:xmpp:private-notes:1`.

The JSON field named `kind` in the reference tools is only a local API argument
that tells the test harness which node role and HKDF domain to use. It is never
serialized into XMPP XML.

## Requirements

The Python tool requires Python 3.10 or newer and `cryptography`:

```sh
python3 -m pip install cryptography
```

The Rust smoke test uses Cargo and the crates declared in `Cargo.toml`. Neither
tool uses Qt.

## Cryptographic constants

All implementations must use exactly:

```text
master key: 32 bytes
key ID: SHA-256(UTF-8("private-notes storage key id v1") || 00 || master key)
HKDF hash: SHA-256
HKDF salt: UTF-8("private-notes HKDF salt v1")
HKDF info: UTF-8("private-notes key domain v1:" || domain)
HKDF output: 32 bytes
index domain: storage-index
content domain: storage-content
cipher: AES-256-GCM
nonce: 12 bytes
GCM tag: 16 bytes
AAD: empty byte string
```

A standard HKDF API receives `info` exactly as shown. It adds the RFC 5869
block counter internally. A manual one-block expansion is:

```text
PRK = HMAC-SHA-256(salt, master_key)
OKM = HMAC-SHA-256(PRK, info || 01)
```

Do not append `01` to `info` before calling a standard HKDF implementation.

Some APIs return `ciphertext || tag`; split the final 16 bytes before writing
the XML. Other APIs expose ciphertext and tag separately. AAD is an empty byte
string in either case.

## Verify the checked-in vectors

From `plugins/xmpppubsub`:

```sh
python3 private-notes-encrypted-reference.py verify-vectors
```

Regenerate and immediately verify them:

```sh
python3 private-notes-encrypted-reference.py generate-vectors \
  --output private-notes-encrypted-vectors.json
python3 private-notes-encrypted-reference.py verify-vectors
```

Generation is deterministic because every fixed vector supplies its nonce.
Production senders must generate a fresh unpredictable 12-byte nonce.

## Encode a record

A basic index request:

```json
{
  "kind": "index",
  "master_key_hex": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "node": "urn:xmpp:private-notes:0:index",
  "item_id": "2b7e1516-28ae-4d2a-abf7-158809cf4f3c",
  "nonce_hex": "000102030405060708090a0b",
  "record": {
    "revision": "018f0be0-df0d-7c70-a2ef-1f5c973ec92a",
    "origin_id": "device-a",
    "title": "Portable note",
    "modified": "2026-07-27T18:00:00.123Z",
    "format": "markdown",
    "tags": ["one", "two"],
    "folder_path": ["Projects", "2026"]
  }
}
```

A content request:

```json
{
  "kind": "content",
  "master_key_hex": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "node": "urn:xmpp:private-notes:0:content",
  "item_id": "2b7e1516-28ae-4d2a-abf7-158809cf4f3c",
  "nonce_hex": "0c0d0e0f1011121314151617",
  "record": {
    "revision": "018f0be0-df0d-7c70-a2ef-1f5c973ec92a",
    "body": "Portable body\n"
  }
}
```

Encrypt the request:

```sh
python3 private-notes-encrypted-reference.py encode request.json \
  --output encoded.json
```

`nonce_hex` is optional for ad-hoc use. When omitted, the reference encoder uses
`os.urandom(12)`. It is present in examples only to make results reproducible.

The result is self-contained and includes:

- the complete PubSub `<item/>` XML;
- the exact reference plaintext XML and its hex representation;
- nonce, ciphertext, and tag in hex/Base64;
- storage key ID in hex/Base64url;
- the derived index or content key;
- the test master key.

Standalone vectors use an unnamespaced `<item/>` wrapper. Inside a complete
XEP-0060 stanza, `<item/>` may inherit
`http://jabber.org/protocol/pubsub`; conformance decoders accept both forms.
Embedding a master key in the output is strictly a test convenience. Production
logs and protocol records must never contain it.

For extension tests, a request may supply `plaintext_xml` instead of `record`.
Those UTF-8 bytes are encrypted as supplied and are still semantically checked
by the decoder.

## Decode and validate a record

```sh
python3 private-notes-encrypted-reference.py decode encoded.json \
  --output decoded.json
```

The decoder verifies:

1. the PubSub item and `urn:xmpp:private-notes:0` namespaces;
2. canonical padded Base64 and unpadded Base64url;
3. the storage key ID;
4. HKDF domain separation selected by the requested node role;
5. AES-256-GCM with empty AAD;
6. well-formed, bounded UTF-8 authenticated XML without DTD/entities;
7. the complete PubSub node binding in `<node>`;
8. exactly one `<index>` or `<note>` appropriate for the actual node;
9. record `id` equality with outer PubSub item ID;
10. required extensions and all core field types/cardinalities.

The key and actual node may be supplied separately instead of using the test
values embedded in `encoded.json`:

```sh
python3 private-notes-encrypted-reference.py decode encoded.json \
  --master-key-hex 000102...1f \
  --node urn:xmpp:private-notes:0:index
```

Known required features can be declared repeatedly:

```sh
--supported-feature urn:example:private-notes:media:1
```

## XML serialization is semantic

There is no XML canonicalization requirement. These differences are compatible:

- namespace prefix choices;
- attribute order;
- single versus double quotes;
- insignificant inter-element whitespace;
- `<element/>` versus `<element></element>`.

Fixed vectors contain exact Python plaintext bytes so another implementation can
validate HKDF and AES-GCM. A native encoder does not need to reproduce those
bytes; it must produce semantically equivalent namespace-aware XML.

A useful two-way interoperability test is:

```text
Python serialize/encrypt -> Rust decrypt/parse/validate
Rust serialize/encrypt   -> Python decrypt/parse/validate
```

The checked-in Rust project implements the first direction as a smoke test.

## Extensions and forward compatibility

Core elements and unqualified core attributes are fixed by the major namespace.
An unknown field in the same core namespace is malformed. Compatible optional
extensions must use another XML namespace, for example:

```xml
<index xmlns='urn:xmpp:private-notes:0'
       xmlns:media='urn:example:private-notes:media:1'
       id='note-id' revision='revision-id'
       modified='2026-07-27T18:00:00.123Z' format='markdown'
       media:preview='available'>
  <title>Portable note</title>
  <folder xmlns='urn:xmpp:private-notes:folders:0'>
    <segment>Projects</segment>
  </folder>
  <media:attachments count='1'/>
</index>
```

A rewriting implementation must preserve unknown optional authenticated
attributes and subtrees. Namespace-aware DOM subtree preservation is the
simplest approach. `urn:xmpp:private-notes:folders:0` is the currently defined
optional folder-path extension: it contains at most one direct `folder` with
one or more trimmed, non-empty `segment` values. A missing folder means an
empty `folder_path` in the reference codec.

`urn:xmpp:private-notes:content:0` is a known **required** extension used
only when an index revision changes without republishing the note body. Its
single `content-revision` child identifies the revision encoded by the content
item. It must differ from the index `revision`; without this extension the
content revision equals the index revision. The reference codec exposes the
resolved value as `content_revision`.

An extension required for safe interpretation is declared inside the
authenticated envelope:

```xml
<required xmlns='urn:xmpp:private-notes:0'
          feature='urn:example:private-notes:media:1'/>
```

An unknown required feature makes the record unsupported and read-only. It is
not malformed and must not be deleted by the maintenance tool.

## Vector file structure

`private-notes-encrypted-vectors.json` contains:

- `protocol_namespace`: the one supported major namespace;
- `folder_namespace` and `content_revision_namespace`: defined extension namespaces;
- `crypto`: normative constants in machine-readable form;
- `positive`: complete requests, encrypted documents, and expected
  semantic results;
- `negative`: documents with one intentional fault and an expected stable
  error category.

Positive cases cover normal index/content records, Unicode, foreign-namespace
extensions, an index-only metadata update, empty optional index data, and an
empty body.

Negative cases cover a legacy pre-unified payload, wrong key, wrong node, moved
item ID, modified GCM tag, missing fields, non-canonical Base64, unknown core
fields, a future major namespace, malformed authenticated XML, an unknown
required extension, and a forbidden DTD/entity declaration.

## Error taxonomy

| Category | Meaning and safe behavior |
| --- | --- |
| `invalid_argument` | Invalid local API request |
| `malformed` | Current-major XML/Base64/core structure is invalid; explicit maintenance may classify it as removable after re-fetching |
| `obsolete` | Positively identified pre-release payload; explicit maintenance may remove it after confirmation and re-fetching |
| `unsupported` | Future major namespace, unknown required extension, or unsupported semantic value; preserve and do not delete |
| `wrong_key` | Outer key ID does not match the configured key; preserve and initiate explicit key resolution when appropriate |
| `authentication_failed` | AES-GCM authentication failed; preserve and do not auto-delete |
| `context_mismatch` | Authenticated node or item binding differs from the actual PubSub context; preserve and report |

One unreadable item must not stop the complete storage. Index listing skips and
reports it while continuing with readable records. A storage-wide error state
is reserved for account, configuration, server-capability, or genuinely global
key failures.

## Rust conformance test

```sh
cd conformance/rust
cargo test
```

To verify an arbitrary Python result:

```sh
cd ..
python3 private-notes-encrypted-reference.py encode request.json \
  --output conformance/rust/encoded.json
cd conformance/rust
cargo run -- encoded.json
```

The Rust code independently parses the PubSub XML, verifies Base64/Base64url,
derives the key, computes the key ID, decrypts AES-256-GCM, compares the exact
reference plaintext bytes, and validates the authenticated node, record type,
and item ID.

It is a smoke consumer, not the complete synchronization implementation.
Production code must additionally preserve unknown XML, enforce resource
limits, implement the full error taxonomy, and isolate individual bad items.

## Cross-language implementation checklist

A new implementation should be able to:

1. reproduce every fixed `key_id_hex` and `derived_key_hex`;
2. split or join `ciphertext || tag` according to its platform API;
3. decrypt every positive vector using empty AAD;
4. parse and validate authenticated XML semantically;
5. reject each negative vector with the documented category;
6. emit equivalent XML using a namespace-aware writer;
7. preserve unknown optional foreign-namespace XML on rewrite;
8. refuse to rewrite unknown required extensions;
9. isolate unreadable individual items instead of stopping the store;
10. use fresh unpredictable nonces in production.
