# Video posters and lightweight thumbnails

Status: **design proposal**, not implemented. This refines
[media storage architecture](media-storage-architecture.md), particularly its
*derived visual cache*, metadata-first synchronization, XMPP SFS and progressive
media sections. It does not change the canonical Markdown representation.

## The problem

The shared `MediaBlock.qml` reserves a visual region for video even when its
remote `previewUrl` cannot be resolved. In that case only the controls remain
visible at the bottom of a tall, empty card. Locally, the first decoded frame
may be black. Remote media should retain its layout and show a useful poster
**before any large video transfer**, without blocking note synchronization.

## Three distinct artifacts

1. **Frame selection metadata**: a source-video-relative
   `posterFrameMs` (optional, clamped to `[0, durationMs/2]` for automatic
   extraction). Record it alongside validated duration and dimensions, keyed
   to the source media identity/revision. A timestamp is a reproducibility
   hint, **not** an alternative to a transmitted thumbnail when the video
   is not locally available. An explicit user-selected poster may use any
   position within the media and overrides the automatic heuristic.
2. **Poster sidecar**: an independently addressed, immutable derived image
   in `LocalMediaStore`, associated with the original `MediaReference`,
   not an extra user-visible Markdown attachment. Target roughly 640px
   width, preserving aspect ratio, bounded encoded size; store the selected
   timestamp and source blob identity/checksum. Store this in manifest-owned
   derived-media metadata; remote stores may publish it as a separately
   encrypted object if supported. It is eligible for mark-and-sweep alongside
   its owning media reference and can be regenerated if the source is local.
3. **Tiny thumbnail**: a second rendition of the same selected frame, target
   160–240px wide and at most **6 KiB encoded**. Prefer JPEG or WebP only where
   recipient support is known; baseline JPEG is the interoperable fallback.
   Preserve aspect ratio. Use a bounded quality/resolution loop, allowing
   low quality rather than exceeding the budget. Small previews should be
   displayed scaled and smoothed (optionally blurred) instead of showing an
   empty card.

Both renditions must come from the **same frame**. A poster is not a persistent
playback frame and must not be regenerated on every play, seek, or note open.

## Selecting a useful frame

- The import/probe service extracts on a worker-bound decoder with its own
  lifetime and strict time/CPU/memory limits; it must not acquire the shared
  foreground `QMediaPlayer` or hold the UI thread.
- Probe candidate timestamps beginning around `min(2s, duration/2)`;
  for a mostly black or low-information frame try a few alternative points,
  e.g. around 0.5s, 3s, or 5s, each clamped to at most half the duration.
  Always use **actually decoded** frame timestamps: seeking is not necessarily
  frame-accurate. Reject empty/invalid frames; compare luminance, contrast,
  and spatial detail with a cheap thumbnail-size analysis.
- Pick the best available candidate, not the first nonblack pixel; dark videos
  are legitimate. If every candidate is dark, retain the best valid image
  instead of inventing content. If decoding is unavailable or times out,
  keep a missing-poster state without blocking note creation/publication.
- Do not export temporary plaintext video frames to persistent filesystem
  paths; import the encoded poster atomically into the existing encrypted
  media-store domain. Preserve `Keep file in place` semantics for the
  original video: poster derivation does **not** move the original file.
- The work is cancellable; repeated import of the same authenticated source
  should reuse previously derived results using source identity and extraction
  policy version.

## XMPP transport and offline availability

The current XMPP backend serializes XEP-0447/SFS descriptors **inside** the
authenticated, encrypted Private Notes content record; video bytes use
XEP-0448 ciphertext served via existing source/offer transports. Poster
transport must preserve these confidentiality boundaries.

1. **Primary bootstrap**: embed a strictly bounded, versioned thumbnail
   rendition (bytes, MIME, width/height, hash, and `posterFrameMs`) in the
   **encrypted Private Notes content record**, or a comparably durable encrypted
   metadata object retrievable while the original device is offline. Keep
   aggregate thumbnail bytes bounded per note (proposed 16 KiB), avoid repeating
   identical rendition hashes, and fall back gracefully when the budget is
   exceeded. This gives a new installation a usable poster without loading
   the large video or relying on the sender being online.
2. **Optional interoperability**: where a conventional SFS descriptor is
   exchanged outside this private note protocol, advertise a
   [XEP-0264 thumbnail](https://xmpp.org/extensions/xep-0264.html)
   with an appropriate `cid:` URI backed by
   [XEP-0231 BoB](https://xmpp.org/extensions/xep-0231.html).
   XEP-0231 recommends **no more than 8 KiB** of binary data; it prefers
   CID advertisement and subsequent IQ retrieval, rather than an unsolicited
   large XML stanza. BoB is a *supplementary compatibility channel*, not
   durable poster storage. Only publish a plaintext thumbnail when the
   thumbnail's disclosure is acceptable: an unencrypted SFS/BoB thumbnail
   can reveal content even when the video uses E2EE.
3. **Optional full poster**: treat the 640px image as an ordinary separately
   encrypted sidecar, with immutable content identity and a source descriptor
   that can be served by peers or HTTP upload. It must never gate access to
   the note or video. The recipient can progressively upgrade its view from
   tiny thumbnail to full poster.

The XMPP backend owns XEP descriptors and transport-specific metadata. The
shared `MediaReference`/derived-media model must not contain XMPP-only
`cid:` or publication details. Keep the original attachment UUID stable;
replacing a thumbnail must not rewrite `anykeep-media:` Markdown links.

**Do not implement a 50 KiB BoB payload.** Base64 makes it larger on the
wire, and both stanza limits and traffic shaping vary by server.

## Rendering contract

Priority on an inactive video card:

1. Full local poster image, if verified and decoded.
2. Tiny local or synchronized thumbnail, scaled to the card
   (`PreserveAspectCrop`, smooth sampling; deliberate blur is optional).
3. Available paused frame **only after** the decoder has produced it.
4. Neutral placeholder surface with a bundled video glyph, title and duration.

Always draw a restrained 1px palette-derived border and modest background
contrast so black frames remain distinguishable in dark themes. Derive card
aspect ratio from authenticated media dimensions when available, otherwise
use **16:9** for a video of unknown dimensions, and never remove the visual
surface merely because `previewUrl` is empty. Controls remain anchored inside
the card at the bottom. The placeholder must not require a running player.

Keep poster visible until the **first valid playback frame** arrives; loading,
seeking and errors should not flash an empty black rectangle. Do not introduce
a second active foreground decoder for every inactive note.

## Durability and trust

- Associate derived image records with an exact original content fingerprint.
  Validate MIME, bounded encoded bytes, decoded pixel count, hash, dimensions,
  and the corresponding note/storage identity before displaying them.
- Do not accept arbitrary external `cid:` or image URLs into a QML `Image`
  source without validation and an explicit resource-resolution policy.
- Persist derived references in draft and remote-cache manifests; GC includes
  their blob IDs with live parents and applies the existing grace period.
  Dropping a cache or losing a poster does not invalidate the original video.
- A regenerated poster should not silently change a user-selected custom
  frame. Automatic generation is a derived metadata update, not a new edit
  to the Markdown text or an undo/redo command.
- Publish note and video descriptors without waiting indefinitely for
  background extraction. If the derived poster arrives later, update only its
  associated metadata with normal revision/concurrency discipline.
- Keep original media publication and key/revision rules unchanged.

## Implementation slices

1. **UI** — fix empty video cards and black-frame boundaries in
   `MediaBlock.qml`, add bundled fallback glyph and tests for remote/no-poster
   and dark-theme states. Independent of network/backend changes.
2. **Core** — cancellable poster-frame extractor and source-fingerprint keyed
   derived artifacts, persist `posterFrameMs` and sidecar identity in the
   manifest (including copy, undo/redo, reroute, GC, and PTF behavior).
   Cover short videos, black openings, missing codecs and interrupted import.
3. **XMPP bootstrap** — encode/decode bounded authenticated thumbnail in
   private note content; receiving installation renders it before hydrating
   video. Test offline sender, multiple videos and aggregate size limits.
4. **Interoperability/quality** — BoB/XEP-0264 CID serving where appropriate,
   optional remotely available full poster sidecar, progressive upgrade and
   cross-device tests. No plaintext thumbnail leak across the E2EE boundary.

## Acceptance checks

- Unknown remote video with no thumbnail never leaves a large blank gap.
- 1–3s and long videos use a bounded useful frame timestamp, within the
  initial half unless explicitly chosen by the user.
- A video beginning with black does not imply a permanently black poster
  when later usable frames exist.
- A new device can render a thumbnail from encrypted note metadata while
  the original device is offline and before video hydration.
- A tiny preview over a slow connection is usable; large BoB stanzas are
  never emitted.
- Video content integrity, key handling, background sync, import/export,
  dark/light themes and Android/desktop playback are not regressed.
