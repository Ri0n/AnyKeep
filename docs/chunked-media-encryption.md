# Chunked media encryption

Status: implementation in progress on `ai/chunked-media-encryption`.

Implementation checkpoint: managed imports write the `AKMC v1` container, `MediaSource` performs authenticated range reads with a one-chunk plaintext cache, and legacy whole-envelope blobs remain readable. The portable remote wire codec, strict XEP-0447 chunked-source descriptor, durable Jingle capability v2, and seekable Jingle range-serving path are now implemented; publication and receive-side range hydration are the remaining XMPP integration steps.

This document refines the chunked-media stage from `media-storage-architecture.md` and separates two concerns that must not be conflated:

1. **AnyKeep local managed storage** — an internal at-rest format fully controlled by AnyKeep. It can switch to independently authenticated chunks without protocol compatibility concerns.
2. **Remote XMPP representation** — current XEP-0448 AES-GCM is a whole-object representation. Progressive AnyKeep media therefore needs a distinct source profile rather than silently redefining XEP-0448.

## Why the distinction matters

XEP-0448 creates one random symmetric key and IV for the file, encrypts the file with the selected cipher, and then transports those encrypted bytes through HTTP Upload or another source. XEP-0234 ranged transfer addresses byte ranges of the file being transferred. Ranging a monolithic AES-GCM ciphertext does not make the corresponding plaintext range independently authenticatable.

Therefore this PR must not silently reinterpret XEP-0448 as per-chunk AES-GCM. Such a representation is a distinct, versioned protocol profile with explicit interoperability rules.

XEP-0447 deliberately permits source types other than the standardized URL/Jingle sources. Iris 1.1 also preserves an unknown source as `Source::Other` with its raw XML. The progressive representation can therefore be an AnyKeep source extension while the nested byte transports remain ordinary XEP-0363 HTTP and XEP-0234/XEP-0358 Jingle.

## Local managed blob format

New managed blobs use a versioned chunk container. Existing whole-envelope `.blob` files remain readable so current local data does not require an eager migration.

Version 1 uses:

- plaintext chunk size: 1 MiB;
- one independently authenticated `SecureEnvelope` per plaintext chunk;
- a small authenticated container header;
- the existing keyed plaintext `blobId` as immutable content identity;
- the existing `LocalMedia` key domain;
- atomic replacement through `QSaveFile`.

Conceptually:

```text
AKMC v1 header
  magic/version
  plaintext size
  chunk size/count
  full-chunk record size
  authenticated header envelope

chunk record 0
  envelope length
  SecureEnvelope(chunk 0)

chunk record 1
  envelope length
  SecureEnvelope(chunk 1)

...
```

The authenticated header binds the container geometry to the `blobId`. Every chunk envelope is additionally bound to:

- `blobId`;
- chunk index;
- total plaintext size;
- chunk size/count;
- expected plaintext size of that chunk;
- container format/schema.

Chunk reordering, truncation, cross-blob substitution, or changing the declared geometry must therefore fail authentication before plaintext from the affected chunk is exposed.

## Random access

`MediaStream` continues to expose plaintext byte offsets. For a managed chunked blob, its `MediaSource` maps a requested plaintext offset to the containing encrypted chunk, authenticates that complete chunk, caches the verified plaintext chunk, and only then copies requested bytes to the consumer.

A decoder seek therefore needs at most the chunks touched by the requested plaintext range rather than decrypting the complete media object. Sequential playback naturally advances chunk-by-chunk.

Legacy whole-envelope blobs still use the existing materializing fallback until they are rewritten or explicitly migrated.

## Import path

Filesystem import is two-pass and bounded-memory:

1. stream the source to compute keyed `blobId`, plaintext SHA-256, and size;
2. if the blob is not already present, rewind the source and write authenticated chunks atomically.

The second pass recomputes both digests and refuses to commit if the source changed, including same-size content changes. `importData()` uses the same chunk writer through a `QBuffer`. External-file references remain external and are not rewritten into managed storage merely because they use the same chunk size for local verification.

## Remote chunked source profile

The intended remote profile is a new optional/required Private Notes media extension, provisionally `urn:xmpp:private-notes:media-chunks:0`. It is carried as an XEP-0447 source of another type, not as an XEP-0448 `<encrypted/>` source. A note using it declares the feature inside the authenticated Private Notes envelope so an older installation does not rewrite a descriptor it cannot interpret.

The source descriptor carries, inside the already encrypted note content:

- profile/version;
- plaintext chunk size;
- a fresh 32-byte representation root key;
- a stable 4-byte nonce prefix;
- complete wire size and SHA-256 when the representation has been fully produced;
- nested ordinary XEP-0363 URL and/or XEP-0358 Jingle publication sources.

The encryption key and nonce prefix MUST NOT appear in the public/private Jingle publication catalog. The catalog only describes the opaque wire object: its size, hash, media type, publication id and provider identity.

### Wire chunk records

The wire object deliberately has no variable header. It is the concatenation of independently authenticated chunk records. Geometry and key material live in the authenticated source descriptor, which makes byte offsets computable without first fetching the beginning of the object.

For each plaintext chunk `i`, construct a fixed 64-byte context:

```text
4   magic = "AKWC"
2   version = 1, big endian
2   reserved = 0
32  plaintext SHA-256 from XEP-0446 metadata
8   total plaintext size, big endian
4   plaintext chunk size, big endian
8   chunk index, big endian
4   actual plaintext bytes in this chunk, big endian
```

The AES-GCM plaintext for a record is:

```text
context[64] || media-bytes[actual-chunk-size]
```

The wire record is:

```text
ciphertext || gcm-tag[16]
```

After decryption the 64-byte context is compared byte-for-byte with the expected values before media bytes are exposed. This authenticates chunk index, whole-file identity and geometry without relying on Qt serialization.

The per-representation AES-256 key is derived from the 32-byte representation root key with the portable Private Notes HKDF profile and a dedicated media-chunk domain. Each chunk uses a 96-bit GCM nonce:

```text
nonce = random-prefix[4] || uint64_be(chunk-index)
```

The root key is fresh for a new immutable representation and the prefix remains stable for retries. A representation MUST never reuse the same root key/prefix pair for different plaintext. Chunk indexes therefore give unique nonces under that derived key. Retry/republication reuses the immutable representation parameters so HTTP and Jingle reproduce identical bytes.

For a full 1 MiB chunk the wire record size is exactly:

```text
1 MiB + 64-byte context + 16-byte tag
```

The final record differs only by the shorter media payload. Therefore:

```text
wireSize = plainSize + chunkCount * 80
wireOffset(chunk i) = i * (chunkSize + 80)
```

for every non-final/full chunk. A desired plaintext chunk maps directly to one complete HTTP/Jingle byte range.

### One virtual encrypted object, multiple transports

The encrypted wire object does not need to be stored as a second full local file. A transport-independent chunk codec can reproduce any record deterministically from:

- the immutable local plaintext `MediaSource` range;
- root key and nonce prefix;
- plaintext SHA-256 and geometry;
- chunk index.

HTTP Upload consumes a sequential `QIODevice` that emits these records in order. The upload pass can compute the complete wire SHA-256 while it streams. Jingle publication later exposes the exact same logical object. On `deviceRequested(offset, size)`, the provider seeks a new `MediaChunkWireStream` directly to the negotiated wire offset. Iris owns the requested range length through its file-transfer `bytesLeft` accounting, so no second range-limiting wrapper or ciphertext copy is required.

This satisfies the original requirement that HTTP and Jingle serve literally identical bytes without requiring AnyKeep to persist a second concatenated ciphertext copy. Persisting encrypted records is only an optional cache.

A Jingle-only publication may require one sequential local pass to compute the complete wire hash before publishing its catalog item. That is CPU/I/O work, not a whole-file memory allocation.

### Receiving ranges

A receiver seeking plaintext chunk `i` requests the complete corresponding wire record through HTTP Range or XEP-0234 `<range/>`, authenticates/decrypts it, validates the embedded fixed context, and installs only the verified plaintext chunk into the ranged local cache. A transport can return arbitrary byte ranges, but the media scheduler should request complete encrypted records because partial GCM records cannot be authenticated.

The existing Iris file-transfer API already exposes negotiated `deviceRequested(offset, size)` in normal mode and a raw `Connection` in streaming mode, so no new XMPP byte transport is required. The new work is the transport-independent wire codec and source XML mapping.

## Compatibility

Existing XEP-0448 whole-object descriptors remain readable and downloadable. They stay non-random-access and may hydrate sequentially before playback. New chunked descriptors use the additional required media-chunks feature.

The first implementation does not claim that the custom representation is XEP-0448. If this format is later standardized, its namespace/framing can be migrated explicitly rather than retroactively changing XEP-0448 semantics.
