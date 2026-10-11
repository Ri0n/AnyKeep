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

**Planned media adapter (not yet implemented):**
- Keep stable `MediaReference` identities in canonical drafts and local
  manifests, but publish native relative Markdown links and native Nextcloud
  file objects so other Notes clients can read them.
- Use Notes attachment API where appropriate for supported server versions;
  use WebDAV and chunked upload for large files, and WebDAV ranged reads
  where supported. Detect API/server capabilities rather than assume a new
  Notes attachment layout works on older installations.
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

However `NextcloudStorage` still advertises `supportsMedia() == false`:
the editor/draft lifecycle and remote MediaReference hydration are not
connected yet. Do not enable media until source/destination publication,
cross-client imports, category moves, crash recovery and cleanup are covered.
The Notes multipart endpoint buffers uploads on the server and is not a
substitute for WebDAV large-file transfers.

## Credential storage

This minimal integration stores the app password in `QSettings`, matching the plugin's other settings.
For a distributable production build, replace that field with a platform keychain integration such as QtKeychain.

## Validation performed

The source layout and overrides were checked against the current public AnyKeep
`master` interfaces. This archive was not fully compiled in the generation
environment because Qt development packages and the complete AnyKeep build tree
were not available there.
