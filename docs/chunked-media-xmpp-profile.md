# Chunked media XMPP profile

This document defines the compatibility boundary for the independently authenticated remote-media representation.
The profile is intentionally **not** XEP-0448. XEP-0448 remains the legacy whole-object AES-GCM representation.

## Feature gate

A content record containing a chunked media source MUST declare the required Private Notes feature
`urn:xmpp:private-notes:media-chunks:0` in addition to the existing media feature.

`XmppNoteCodec` treats the feature as content-only and enforces both directions:

- a chunked source without the required feature is corrupt;
- the required feature without a chunked source is corrupt.

This prevents an older implementation that does not understand the profile from decrypting, editing, and then silently
rewriting a media descriptor whose semantics it cannot preserve.

The source is a direct XEP-0447 source inside the ordinary `<sources xmlns='urn:xmpp:sfs:0'>` container. Its provisional
element name is `<encrypted-chunks xmlns='urn:xmpp:private-notes:media-chunks:0'>`. Transport sources nested below that
profile remain ordinary HTTP Upload and/or Jingle publication sources.

## Secret and public metadata boundary

The authenticated Private Notes content may carry the representation root key, nonce prefix, chunk geometry and optional
complete wire hash. Those values are part of the encrypted note descriptor.

The Jingle publication catalog is different: it advertises only the opaque wire object and MUST NOT expose the
representation root key or nonce prefix. Durable local Jingle capabilities are encrypted at rest.

The durable capability store uses an explicit representation discriminator rather than interpreting missing fields:

- `LegacyXep0448` stores the XEP-0448 cipher, key and IV;
- `ChunkedAnyKeep` stores `MediaChunkWireParameters`.

Store version 2 reads version 1 records as `LegacyXep0448`, preserving existing published media authority across the
upgrade.

## Transport identity

For one immutable chunked representation, HTTP and Jingle reproduce the same virtual encrypted byte object. A full wire
SHA-256 may be persisted when a sequential HTTP pass has produced it. When no precomputed hash exists, Jingle may use
XEP-0300 `hash-used` and compute the transfer checksum incrementally instead of forcing an O(file-size) preparation pass.
