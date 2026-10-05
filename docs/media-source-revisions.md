# External media source revisions

This note defines the local-only identity and integrity model for media that the
user chooses to keep at its original filesystem location.

## Source identity vs. content identity

An external media attachment has two independent identities:

- **attachment identity** — the stable `MediaReference::id` UUID used by
  `anykeep-media:` URIs and document structure;
- **content identity** — the keyed `blobId` plus plaintext SHA-256, computed
  lazily from the file bytes.

The attachment identity exists immediately. A newly linked external file may
therefore have an empty `blobId` and checksum until content identity is actually
needed for publication, promotion into managed storage, or an explicit integrity
operation.

Filesystem location and revision metadata are never portable `MediaReference`
fields. They live in an encrypted LocalMediaStore source record keyed by the
attachment UUID.

## SourceRevision

The cheap revision guard currently consists of:

- canonical filesystem path;
- file size;
- modification timestamp.

The basename is retained as normal attachment metadata. A platform file ID
(device/inode on Unix, file ID on Windows) may be added later to strengthen the
revision guard without changing the portable note model.

`SourceRevision` is deliberately not a cryptographic content identity. It answers
only whether a previously computed fingerprint is still safe to reuse without a
new full-file scan.

## Lazy fingerprint cache

When content identity is required, LocalMediaStore reads the source in fixed 1 MiB
chunks and computes in one pass:

- keyed HMAC-SHA-256 `blobId`;
- plaintext SHA-256;
- SHA-256 for every fixed-size chunk.

The source revision is checked before and after the pass. Results are persisted in
the encrypted source record only if the revision remained stable.

A cached fingerprint may be reused only while the current size and modification
timestamp still match the recorded revision. A revision change invalidates the
whole fingerprint and causes the next integrity-requiring operation to recompute
it. Once a fingerprint exists, random-access playback also verifies each complete
chunk before exposing its bytes. This detects same-size/same-timestamp content
replacement when an affected chunk is read.

Linking an external file itself is metadata-only and must not perform a full-file
scan. The first expensive fingerprint operation runs off the application thread.

## Jingle and HTTP hashes

The plaintext SHA-256 is the logical XEP-0234/XEP-0446 file hash. It is independent
of any transport encryption.

For a Jingle-only XEP-0448 source AnyKeep does not precompute the SHA-256 of the
entire ciphertext. The durable JinglePub offer advertises XEP-0300 `hash-used`
with SHA-256. Iris hashes the actual ciphertext incrementally while Jingle File
Transfer is running and exchanges the resulting checksum through the normal
XEP-0234 signaling path.

The encrypted-source hash in XEP-0448 is optional. If HTTP Upload actually sends
the ciphertext, its hash is already available as a by-product of that upload and
may be retained. A failed or unavailable HTTP path must not cause a second
whole-file encryption/hash pass merely to prepare Jingle.

The receiver always retains the stronger end-to-end checks: transport/Jingle
checksum when negotiated, AES-GCM authentication of the encrypted payload, and
plaintext size/SHA-256 verification before accepting the logical file.

## Future chunked wire representation

The AnyKeep chunked remote profile authenticates each encrypted record
independently. Whole-representation hashes may be accumulated as a streaming
by-product but are not a prerequisite for starting playback or transfer. This is
the basis for ranged HTTP and Jingle delivery without O(file-size) preparation
latency.
