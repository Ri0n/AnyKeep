# Chunked media encryption

Status: implementation in progress on `ai/chunked-media-encryption`.

Implementation checkpoint: managed imports now write the `AKMC v1` container, `MediaSource` performs authenticated range reads with a one-chunk plaintext cache, and legacy whole-envelope blobs remain readable. The first CI pass exercises this local-storage boundary before remote wire-format work starts.

This document refines the chunked-media stage from `media-storage-architecture.md` and separates two concerns that must not be conflated:

1. **AnyKeep local managed storage** — an internal at-rest format fully controlled by AnyKeep. It can switch to independently authenticated chunks without protocol compatibility concerns.
2. **Remote XMPP representation** — currently XEP-0448 AES-GCM over one encrypted file. This profile remains a whole-object representation until a compatible seekable/chunked representation is explicitly defined.

## Why the distinction matters

XEP-0448 creates one random symmetric key and IV for the file, encrypts the file with the selected cipher, and then transports those encrypted bytes through HTTP Upload or another source. XEP-0234 ranged transfer addresses byte ranges of the file being transferred. Ranging a monolithic AES-GCM ciphertext does not make the corresponding plaintext range independently authenticatable.

Therefore this PR must not silently reinterpret XEP-0448 as per-chunk AES-GCM. Such a representation would require a distinct, versioned protocol profile/extension and explicit interoperability rules.

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

Filesystem import becomes two-pass and bounded-memory:

1. stream the source to compute keyed `blobId`, plaintext SHA-256, and size;
2. if the blob is not already present, rewind/reopen the source and write authenticated chunks atomically.

`importData()` uses the same chunk writer through a `QBuffer`. External-file references remain external and are not rewritten into managed storage merely because they use the same chunk size for local verification.

## Remote follow-up

After local chunked storage is stable, the remote layer can be addressed separately:

- Jingle FT ranges operate on byte ranges of the advertised transferred object;
- HTTP Range likewise operates on the uploaded object;
- current XEP-0448 remains one whole encrypted object and therefore cannot provide independently authenticated plaintext random access merely by adding ranges;
- a future chunked remote representation must define its own versioned framing, key/nonce rules, authenticated metadata, ciphertext byte layout, range mapping, and compatibility behavior before AnyKeep advertises it as a remote source.

Until that protocol is defined, local chunked encryption is an at-rest/random-access improvement and not a wire-format change.
