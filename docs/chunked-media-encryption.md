# Chunked media encryption

Status: progressive XMPP publication and playback implemented on `ai/chunked-media-encryption`; ICE/SCTP deployment also requires the QCA DTLS queue fix described below.

Managed imports use the `AKMC v1` container; local and remote reads authenticate complete chunks before exposing plaintext. New XMPP attachments publish a durable Jingle capability and encrypted chunk descriptor without uploading the complete file. Receiving a note returns its body and attachment metadata immediately. Playback, image previews, open/save and seeks resolve missing ranges on demand through the existing Iris publication/file-transfer stack.

## Implemented publication and playback path

- Local import/fingerprinting runs outside the UI thread. A new external file still needs one bounded-memory plaintext identity pass; QCA SHA-256/HMAC acceleration avoids the slow software-only hashing path. Cached unchanged identities can be reused.
- New chunked attachments use Jingle publication. HTTP Upload is not a prerequisite and its common 10–20 MiB limit does not restrict this path. Existing URL sources can still supply authenticated HTTP ranges.
- Jingle uses the existing Iris transport selection, which prefers ICE/DTLS/SCTP when both peers support it and retains S5B/IBB as fallback candidates. Only one ranged transfer runs at a time per backend; read-ahead is bounded to four complete records (normally about 4 MiB).
- The generic `MediaRangeService` supplies an opaque localhost URL to Qt consumers. This is an in-process decoder adapter, not an XMPP HTTP upload service. Reads execute on the backend owner's thread; responses return on the application thread, with bounded socket buffers and cancellation when the owner disappears.
- Persistent range cache files contain encrypted records, authenticated again when read. One verified plaintext chunk is cached in memory. A cached range works offline; missing or corrupt ranges require an available source and fail without exposing unverified bytes.
- Remote open/save streams plaintext to an atomic output file and checks total size and SHA-256 before committing. Editing note text preserves existing remote attachment descriptors without downloading the attachment.

## Deployment and current limits

The current Iris 1.1.2 release (commit `d0586833`) contains the accompanying own-resource capability, FT checksum/receipt lifetime, retained socket and ordered shutdown fixes. The AnyKeep Qt Creator build continues to use its configured system Iris.

QCA 3.0.9 still needs a DTLS input-queue fix: when several datagrams arrive while a provider update or readiness action is pending, the connected TLS layer must continue processing the queued datagrams without requiring another network event. Otherwise packets remain inside QCA, SCTP retransmits and backs off, and larger ranges can time out. A local regression delivers 32 encrypted datagrams as one burst: unpatched QCA delivers only 2, the fix delivers all 32 in order. Live validation uses a separately built QCA via runtime library/plugin paths; the system installation is unchanged.

There is no automatic complete background mirror yet. An uncached part requires the publishing device to remain online and reachable. Both S5B and ICE/SCTP have been validated locally with real progressive video playback and seek; this fixture does not cover real WAN/NAT traversal. Legacy XEP-0448 whole-object attachments still authenticate the whole object before exposing plaintext.

Validation used two separate processes/accounts resources through local Prosody, a 2,343,036,928-byte AVI, real Qt/FFmpeg decoding, exact start/middle/end comparisons, and a seek to the movie's midpoint. The opt-in `irisprogressivemedialive_test` additionally checks disconnected cache reads and tamper rejection. Set `ANYKEEP_LIVE_TRANSPORT=ice` on both fixture processes to exclude S5B/IBB and assert ICE negotiation; leave it unset to exercise normal transport selection. Default CTest skips the external live fixture.

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

The Jingle-only path does not precompute a complete ciphertext hash: the representation is virtual, and independently authenticated records bind the whole plaintext identity and geometry. Iris negotiates checksums for each transferred range. A full wire hash is supplied only when a complete representation has actually been produced.

### Receiving ranges

A receiver seeking plaintext chunk `i` requests the complete corresponding wire record through HTTP Range or XEP-0234 `<range/>`, authenticates/decrypts it, validates the embedded fixed context, and installs only the verified plaintext chunk into the ranged local cache. A transport can return arbitrary byte ranges, but the media scheduler should request complete encrypted records because partial GCM records cannot be authenticated.

The existing Iris file-transfer API already exposes negotiated `deviceRequested(offset, size)` in normal mode and a raw `Connection` in streaming mode, so no new XMPP byte transport is required. The new work is the transport-independent wire codec and source XML mapping.

## Compatibility

Existing XEP-0448 whole-object descriptors remain readable and downloadable. They stay non-random-access and may hydrate sequentially before playback. New chunked descriptors use the additional required media-chunks feature.

The first implementation does not claim that the custom representation is XEP-0448. If this format is later standardized, its namespace/framing can be migrated explicitly rather than retroactively changing XEP-0448 semantics.
