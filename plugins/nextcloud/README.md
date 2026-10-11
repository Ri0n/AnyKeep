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
manifest for an **existing note with a known ETag**. It verifies local
managed/external contents (up to 16 MiB per file), creates the native
`.attachments.<noteId>/` directory through WebDAV, and stages each file
with chunked-v2 upload and atomic no-overwrite MOVE. The filename includes
the stable local UUID and a content digest. A bounded range read-back
must match the source bytes before the code updates the ordinary Markdown
body with `If-Match`. Repeated saves verify/reuse the existing remote
receipt; previously staged identical uploads are also recoverable by
their deterministic filename. Native-client edits to an attachment cause a
conflict rather than a silent replacement. Locally the saved note retains
canonical URIs and `nextcloud.noteId`/`nextcloud.path` receipts.

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
and older Notes API sibling URLs, downloads small nonempty files in WebDAV
byte ranges, imports them into encrypted `LocalMediaStore`, and restores
canonical local Markdown URIs. Each file is limited to 16 MiB; at most
12 attachments and 32 MiB of plaintext are restored per note. Unavailable
or oversized remote links are preserved verbatim, never truncated or
deleted. Remote HTTP replies are capped even if the server ignores Range.
Real Nextcloud accounts require HTTPS; only loopback HTTP fixtures are
allowed. No custom Nextcloud E2EE is applied.

**Not yet a complete media feature:** creating a new note with attachments
still needs an idempotent, crash-recoverable remote ID; streaming from a
local encrypted source rather than whole-object materialization; progressive
large-file playback, remote-only manifests, zero-byte media, deferred
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
