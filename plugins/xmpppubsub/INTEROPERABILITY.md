# QtNote encrypted PubSub XML interoperability guide

This guide is the operational companion to `PROTOXEP.md`. It explains how an
independent implementation can reproduce the cryptography, encrypt arbitrary
records with the Python reference tool, decrypt them with another language,
and compare semantic results without depending on Qt.

## Files

| File | Purpose |
| --- | --- |
| `PROTOXEP.md` | Normative protocol description |
| `qtnote-encrypted.xsd` | Reference schema for the outer encrypted element |
| `qtnote-storage.xsd` | Reference schema for authenticated envelope 1.0 |
| `qtnote-note.xsd` | Reference schema for index/content records 1.0 |
| `qtnote-encrypted-reference.py` | Independent Python encoder, decoder, and vector generator |
| `qtnote-encrypted-vectors.json` | Fixed positive and negative conformance cases |
| `conformance/rust/` | Independent Rust decryption and XML validation smoke test |

The XSD files describe the current 1.0 core. A higher compatible minor or an
independent extension namespace can legitimately contain additional XML that a
1.0 schema does not recognize. Schema validation is not a replacement for the
major/minor and unknown-field preservation rules.

## Requirements

The Python tool requires Python 3.10 or newer and `cryptography`:

```sh
python3 -m pip install cryptography
```

The Rust smoke test uses Cargo and the crates declared in its `Cargo.toml`.
Neither tool uses Qt or QtNote production code.

## Cryptographic constants

All implementations must use exactly:

```text
master key: 32 bytes
key ID: SHA-256(UTF-8("QtNote storage key id v1") || 00 || master key)
HKDF hash: SHA-256
HKDF salt: UTF-8("QtNote HKDF salt v1")
HKDF info: UTF-8("QtNote key domain v1:" || domain)
HKDF output: 32 bytes
index domain: storage-index
content domain: storage-content
cipher: AES-256-GCM
nonce: 12 bytes
GCM tag: 16 bytes
AAD: empty byte string
```

A standard HKDF API receives the `info` value exactly as shown. It adds the RFC
5869 block counter internally. A manual one-block expansion is:

```text
PRK = HMAC-SHA-256(salt, master_key)
OKM = HMAC-SHA-256(PRK, info || 01)
```

Do not append `01` to `info` before calling a standard HKDF implementation.

## Verify the checked-in vectors

From `plugins/xmpppubsub`:

```sh
python3 qtnote-encrypted-reference.py verify-vectors
```

Regenerate and immediately verify them:

```sh
python3 qtnote-encrypted-reference.py generate-vectors \
  --output qtnote-encrypted-vectors.json
python3 qtnote-encrypted-reference.py verify-vectors
```

Generation is deterministic because every fixed vector supplies its nonce.
Production senders must generate a fresh unpredictable 12-byte nonce.

## Encode a record

A basic index request:

```json
{
  "kind": "index",
  "master_key_hex": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "node": "urn:xmpp:qtnote:notes:0:index:1",
  "item_id": "2b7e1516-28ae-4d2a-abf7-158809cf4f3c",
  "nonce_hex": "000102030405060708090a0b",
  "record": {
    "revision": "018f0be0-df0d-7c70-a2ef-1f5c973ec92a",
    "origin_id": "device-a",
    "title": "Portable note",
    "modified": "2026-07-27T18:00:00.123Z",
    "format": "markdown",
    "tags": ["one", "two"]
  }
}
```

A content request:

```json
{
  "kind": "content",
  "master_key_hex": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
  "node": "urn:xmpp:qtnote:notes:0:content:1",
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
python3 qtnote-encrypted-reference.py encode request.json \
  --output encoded.json
```

`nonce_hex` is optional for ad-hoc use. When omitted, the reference encoder uses
`os.urandom(12)`. It is present in examples only to make results reproducible.

The output is self-contained and includes:

- the complete PubSub `<item/>` XML;
- the exact reference plaintext XML and its hex representation;
- nonce, ciphertext, and tag in hex/Base64;
- storage key ID in hex/Base64url;
- derived index/content key;
- the test master key.

Standalone vectors use an unnamespaced `<item/>` wrapper. Inside a complete
XEP-0060 stanza, the item can inherit `http://jabber.org/protocol/pubsub`;
conformance decoders accept both forms.

Embedding the master key in the output is strictly a conformance convenience.
Production logs or protocol records must never contain it.

For advanced extension tests, a request may supply `plaintext_xml` instead of a
`record` object. The supplied XML bytes are encrypted exactly as UTF-8 and are
still semantically validated by the decoder.

## Decode and validate a record

```sh
python3 qtnote-encrypted-reference.py decode encoded.json \
  --output decoded.json
```

The decoder checks:

1. outer PubSub XML structure and namespaces;
2. canonical Base64 and unpadded Base64url encodings;
3. supported outer wire/schema major versions;
4. storage key ID;
5. HKDF domain separation;
6. AES-256-GCM with empty AAD;
7. UTF-8 authenticated XML structure;
8. authenticated wire/schema equality with the outer values;
9. complete PubSub node binding;
10. record type and outer `kind`;
11. record ID and outer item ID;
12. required extensions and known record fields.

The master key and actual PubSub node may be supplied separately instead of
using the test values embedded in `encoded.json`:

```sh
python3 qtnote-encrypted-reference.py decode encoded.json \
  --master-key-hex 000102...1f \
  --node urn:xmpp:qtnote:notes:0:index:1
```

Known required features can be declared repeatedly:

```sh
--supported-feature urn:example:qtnote:media:1
```

## XML serialization is semantic

There is no XML canonicalization requirement. These differences are compatible:

- namespace prefix choices;
- attribute order;
- single versus double quotes;
- insignificant inter-element whitespace;
- `<element/>` versus `<element></element>`.

Text content, namespaces, attributes, cardinalities, and authenticated bindings
are protocol data. Fixed vectors contain exact Python plaintext bytes so another
implementation can validate AES-GCM, but its own encoder does not have to
reproduce those bytes.

A useful two-way interoperability test is:

```text
Python serialize/encrypt -> Rust decrypt/parse/validate
Rust serialize/encrypt   -> Python decrypt/parse/validate
```

The checked-in Rust project implements the first direction. A complete second
implementation should add the reverse direction using its native XML writer.

## Extension preservation

A compatible reader may encounter a higher minor version and unknown optional
attributes or elements. If it rewrites that record, it must preserve those
unknown authenticated XML nodes. Namespace-aware DOM subtree preservation is
the simplest approach.

An unknown declaration such as:

```xml
<required xmlns='urn:xmpp:qtnote:storage:1'
          feature='urn:example:qtnote:media:1'/>
```

means the record cannot be safely interpreted or rewritten without that
feature. It is `unsupported`, not malformed, and must not be removed by the
maintenance tool.

## Vector file structure

`qtnote-encrypted-vectors.json` contains:

- `crypto`: normative constants repeated in machine-readable form;
- `positive`: five complete encoder requests, encrypted documents, and expected
  semantic decoder results;
- `negative`: documents with one intentional fault and a stable error category.

Positive cases cover:

- normal index and content records;
- Unicode and optional XML extensions;
- higher compatible wire/schema minor versions;
- empty optional index data;
- an empty body.

Negative cases cover:

- wrong storage key;
- wrong actual PubSub node;
- moved item ID;
- modified GCM tag;
- missing outer fields;
- non-canonical Base64;
- future wire/schema major versions;
- unauthenticated outer-version tampering;
- malformed authenticated XML;
- unknown required extension;
- forbidden DTD/entity declarations.

## Error taxonomy

| Category | Meaning and safe behavior |
| --- | --- |
| `invalid_argument` | The caller supplied an invalid local request |
| `malformed` | Current-major XML/Base64/field structure is structurally invalid; explicit maintenance may classify it as removable after re-fetching |
| `obsolete` | Positively identified pre-XML development payload; explicit maintenance may remove it after confirmation and re-fetching |
| `unsupported` | Future major, unknown required extension, or unsupported semantic value; preserve and do not delete |
| `wrong_key` | Outer key ID does not match the configured key; preserve and initiate explicit key resolution when appropriate |
| `authentication_failed` | GCM authentication or authenticated outer/inner version comparison failed; preserve and do not auto-delete |
| `context_mismatch` | Authenticated node or item binding differs from the actual PubSub context; preserve and report |

One unreadable item must not stop the complete storage. Index listing skips it,
reports the condition, and continues with all readable records. Only a global
account/configuration/server failure, or an all-record key mismatch requiring
key recovery, should move the storage into a storage-wide error state.

## Rust conformance test

```sh
cd conformance/rust
cargo test
```

To verify an arbitrary Python result:

```sh
cd ..
python3 qtnote-encrypted-reference.py encode request.json \
  --output conformance/rust/encoded.json
cd conformance/rust
cargo run -- encoded.json
```

The Rust code independently:

- parses the complete PubSub XML;
- verifies canonical Base64/Base64url fields;
- derives the key with HKDF-SHA-256;
- computes the storage key ID;
- decrypts AES-256-GCM with empty AAD;
- compares exact reference plaintext bytes;
- parses the authenticated XML;
- verifies versions, node, record kind, and item ID.

It is a smoke consumer, not the full QtNote implementation. Production code
must additionally preserve unknown XML, enforce resource limits, implement all
error categories, and support the synchronization lifecycle in `PROTOXEP.md`.

## Cross-language implementation checklist

A new implementation should be able to:

1. reproduce every `key_id_hex` and `derived_key_hex` value;
2. split or join `ciphertext || tag` according to its platform API;
3. decrypt every positive vector using empty AAD;
4. parse and validate the authenticated XML semantically;
5. reject each negative vector with the documented category;
6. emit equivalent XML with its native namespace-aware XML writer;
7. preserve unknown optional authenticated attributes/elements on rewrite;
8. refuse to rewrite unknown required extensions;
9. isolate unreadable individual items instead of stopping the entire store;
10. use fresh unpredictable nonces in production.
