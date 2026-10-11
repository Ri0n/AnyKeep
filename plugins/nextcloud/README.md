# AnyKeep Nextcloud Notes storage

This directory is intended to be copied to `AnyKeep/plugins/nextcloud`.

## Integration

1. Add `nextcloud` to `plugins_list` in `plugins/CMakeLists.txt`.
2. Reconfigure CMake and build AnyKeep.
3. Enable the plugin and enter:
   - the base Nextcloud URL, including any installation subdirectory;
   - the Nextcloud user name;
   - an app password;
   - a request timeout.

Example server URL:

```text
https://cloud.example.com
```

For a subdirectory installation:

```text
https://example.com/nextcloud
```

The code appends `/index.php/apps/notes/api/v1`.

## Behavior

- `noteList()` downloads metadata only; note content is loaded lazily.
- Network requests run in a dedicated worker thread.
- The asynchronous storage API executes network requests in the worker thread and reports completion through jobs.
- Updates send `If-Match` with the last known note ETag.
- HTTP 412 is treated as a conflict. The local text is preserved and the server version is not overwritten.
- Server-sanitized titles are adopted from the API response.
- `category`, `readonly`, and `etag` are preserved as opaque backend attributes of the generic note data.
- `favorite` is exposed through the generic note API and can be changed directly from the note editor.
- There is no persistent offline cache and no three-way merge in this first version.

## Interoperability and encryption policy

Nextcloud Notes is an **interoperable, server-readable storage**. Do not apply
AnyKeep's XMPP end-to-end encryption to Nextcloud note bodies, attachment
payloads, paths or filenames. Server-side Notes clients (web, desktop and
mobile) must be able to open and edit the same Markdown and attachments.
Use HTTPS (except for an isolated loopback test server); Nextcloud's own
server-side encryption and access policies are independent of AnyKeep.

Local AnyKeep draft/media/cache encryption **remains enabled**. Decrypt locally
at the provider boundary before publishing a file to Nextcloud; do not upload
AnyKeep's internal encrypted blob format or `anykeep-media:` URI verbatim as
the only attachment representation.

**Media adapter requirements (partially implemented):**
- Keep stable `MediaReference` identities in canonical drafts and local
  manifests, but publish native relative Markdown links and native Nextcloud
  file objects so other Notes clients can read them.
- Preserve the web client's per-note attachment layout (`.attachments.<noteId>/`)
  through WebDAV for servers running Notes 5.0.2 and older. WebDAV chunked v2
  uploads and HTTP Range reads are transport primitives; the v1.4 attachment
  API remains available for compatibility and migration only.
- Detect API/server capabilities rather than assume a new Notes attachment
  layout works on older installations.
- Download/import native attachments into AnyKeep media references without
  changing the source note unexpectedly. Preserve content when moving
  between categories and avoid orphaned files during deletion or failed saves.
- Preserve the remote note/attachment publication contract: never claim
  a new attachment is durably published until its remote object is available.
- Test with live Nextcloud Notes and a native-client-compatible Markdown
  representation, including cross-client edit roundtrips.

**Implementation in progress:** `NextcloudWorker` now has native Notes v1.4
multipart attachment upload/download, WebDAV chunked v2 upload with an
atomic final MOVE, capability/settings lookup, and bounded HTTP Range reads.
`NextcloudMediaLinks` translates canonical URI targets to safe native Markdown
paths. The disposable-server live test exercises these HTTP operations.

An internal `NextcloudStorage::saveNoteAsync()` path accepts a media
manifest for an **existing note with a known ETag**, or creates a new
note via a crash-recoverable placeholder transaction. It authenticates and hashes local
managed/external content through `MediaSource`/`MediaStream` in the worker
thread without materializing the whole plaintext (up to the 50,000-chunk
WebDAV upload bound, 48.8 GiB), and creates the native
`.attachments.<noteId>/` directory through WebDAV, and stages each file
with chunked-v2 upload and atomic no-overwrite MOVE. The filename includes
the stable local UUID and a content digest. A bounded Range read-back
is compared chunk by chunk against the same authenticated source; no
whole-object `QByteArray` crosses threads. The original SHA-256 is rechecked
before upload so a changed external file does not inherit a stale
deterministic filename. Older pre-chunking managed blobs still use the
legacy whole-object decoder in `MediaSource`; publication rejects those
legacy objects above 16 MiB until a chunked local migration is available.
Modern managed chunks and lazy external files remain stream-backed. Remote bytes must match before updating Markdown
body with `If-Match`. Repeated saves verify/reuse the existing remote
receipt; previously staged identical uploads are also recoverable by
their deterministic filename. Native-client edits to an attachment cause a
conflict rather than a silent replacement. Locally the saved note retains
canonical URIs and `nextcloud.noteId`/`nextcloud.path` receipts.

### New notes with media: durable identity and ambiguous POST

Nextcloud Notes assigns a server note ID during POST. If a response is lost,
blindly repeating POST can create duplicate notes. For an initial media save,
AnyKeep authenticates the local media **before** writing a durable, account-
scoped creation journal (a hashed server URL/account plus a stable UUID,
with an attempt flag and eventual remote ID; no note plaintext or credentials).
`DraftManager` passes the persisted encrypted `DraftRecord.id` as
`anykeep.publicationDraftId`. A direct `createNote()` caller instead
receives a session `nextcloud.createToken`; full restart safety for user
drafts relies on the durable DraftRecord.

Only after journaling an attempt does the worker issue exactly one Notes POST
with a temporary `# AnyKeep pending media <UUID>` heading and
`<!-- anykeep-media-create:<UUID> -->` marker. Once the server ID is known,
the worker persists it, stages verified WebDAV files into
`.attachments.<noteId>/`, and commits native Markdown with If-Match.
Successful saves return the original canonical text and media manifest.

On restart/retry with no saved remote ID, the worker searches Notes metadata
for that specific marker, fetches matching candidates to verify the exact
body marker, and resumes the existing note. If it cannot yet establish the
identity, it **does not send another POST**: the encrypted draft remains
recoverable, but automatic publication may pause indefinitely until the
ambiguous outcome can be resolved. Once a remote ID is journaled, a retry
can also detect a previously completed Markdown update and verify the
already-staged attachments without another write. Unrelated native-client
edits are conflicts, not overwritten.

A failed/crashed upload may leave a visible, temporary marker note or
unreferenced native WebDAV files. There is deliberately no automatic
delete/cleanup until reference-aware reconciliation exists. Creation-journal
entries are retained as duplicate-prevention tombstones until a future
durable ACK/garbage-collection protocol can safely prune them.

**Confirmed Notes 5.0.2 interoperability trap:** uploads through its native
v1.4 attachment API are placed alongside the Markdown note, and the
server does **not** move those sibling files when the note changes category.
A real-server test reproduced HTTP 404 for the attachment after a move.
The web client's `.attachments.<noteId>/` directory follows the note and
is used by this adapter for safe publication. The upstream Notes fix
([PR #1998](https://github.com/nextcloud/notes/pull/1998)) was merged in
August 2026, after Notes 5.0.2. Metadata-only category moves reject legacy
relative links instead of silently breaking them. A successful move also
preserves the local canonical manifest when the response includes the body.

A bounded `loadNoteAsync()` path recognizes both web-native per-note URLs
and older Notes API sibling URLs. Nonempty files up to 16 MiB can be
downloaded in bounded WebDAV ranges into encrypted `LocalMediaStore`
(up to 32 MiB eager data per note). Large files and files exceeding the
remaining eager budget become **remote-only** `MediaReference` entries:
a one-byte 206 probe provides their size and opaque WebDAV ETag, and the
canonical Markdown URI is restored immediately without downloading the
whole video. Up to 48 referenced native paths are considered during load.

Remote-only playback and image preview reuse the shared
`MediaRangeService` local HTTP bridge and Nextcloud's worker-thread WebDAV
Range reader; the request is capped at 1 MiB and pins `If-Match` to the
initial ETag. Each missing 1 MiB plaintext chunk is fetched once and
committed atomically via the **shared `MediaPartialChunkCache`**, sealed
locally with `SecureEnvelope` and the profile's existing media key.
Only authenticated local chunks are returned on cache hits, so repeated
seeks and restarts do not require the remote server for already cached
ranges. A changed revision, wrong response size, missing/weak ETag,
untrusted path or changed account fails closed. Account identity is
SHA-256-scoped in the manifest and the cache's AEAD context binds the
source revision, size, geometry and chunk index: **no app password, bearer
URL or unencrypted remote payload is saved on disk**. The cache key is
resolved on the application thread before dispatching worker operations;
a locked keychain does not prevent displaying the note, but prevents
hydrating or reading its encrypted partial media.

XMPP uses this same partial-cache class in verified-opaque mode, preserving
existing XMPP encrypted wire-chunk files without re-encrypting or relocating
them. Nextcloud uses locally sealed mode, because native Notes attachments
remain plain HTTPS/WebDAV bytes for other clients.

This is **not** a full background mirror: there is no automatic complete-file
prefetch scheduler, cache eviction policy, zero-byte WebDAV range fallback,
or new-note publication directly from an incomplete remote-only reference.
The current remote-only transport fetches missing chunks on demand and
reports only authenticated, durably committed bytes in `MediaSyncService`.
Links that cannot be probed or pinned remain native Markdown. WebDAV
replies stay capped even when a server ignores Range.
Real Nextcloud accounts require HTTPS; only loopback HTTP fixtures are
allowed. No custom Nextcloud E2EE is applied.

**Not yet a complete media feature:** manual resolution for an ambiguous POST
whose marker never becomes visible, automatic prefetch/eviction and
partial-object publication; zero-byte media, deferred
reference-aware cleanup of abandoned/stale attachments, safe legacy
attachment migration before category moves, and multi-device end-to-end
verification. A failed Markdown update may leave unreferenced staged files
but keeps the durable local draft untouched. The synchronous save method
still rejects media and `supportsMedia()` stays false.

## Credential storage

This minimal integration stores the app password in `QSettings`, matching the plugin's other settings.
For a distributable production build, replace that field with a platform keychain integration such as QtKeychain.

## Validation

The branch has GitHub Actions runs for cross-platform compilation/QtTest and
a disposable real Nextcloud 32 / Notes 5.0.2 server. Check the **current
branch HEAD**, not just a previously successful transport-only run. Before
enabling media, add end-to-end tests covering native-client edits, multiple
devices, retries, category moves, cleanup and progressive playback.
