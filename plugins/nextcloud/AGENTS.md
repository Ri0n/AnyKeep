# Nextcloud Notes plugin map

Read `README.md` for the HTTP/API behavior and user-facing configuration.

## Ownership

| Area | Owner |
| --- | --- |
| Plugin registration/lifetime | `nextcloudplugin.*` |
| `NoteStorage` API, cache, jobs, configuration, folder reconciliation | `nextcloudstorage.*` |
| HTTP requests, JSON codec, ETag/412 handling | `nextcloudworker.*` |
| Category/path conversion | `nextcloudcategory.*` |
| Cross-thread request/result values | `nextclouddto.h` |

## Invariants

- `NextcloudWorker` lives in its dedicated worker thread; storage-facing job
  completion returns to the storage object's thread.
- Preserve lazy content loading, opaque remote attributes, ETag preconditions,
  and HTTP 412 conflict behavior.
- Keep remote category/path conversion in `nextcloudcategory.*`; generic folder
  identity and assignments remain owned by the shared folder catalog.
- Keep successful local writes/removals visible while eventually consistent
  list responses catch up.
- Do not bypass Qt network TLS validation or log app passwords/note content.
- Nextcloud is deliberately **not** an AnyKeep E2EE backend. Keep note bodies
  and attachments readable by the native Nextcloud Notes clients: no custom
  ciphertext, encrypted filenames, or `anykeep-media:`-only remote markup.
  Keep local draft/media/cache at-rest encryption as-is and publish native
  plaintext files over HTTPS.
- While media support is disabled, reject unresolved canonical `anykeep-media:`
  links and incomplete media publication. An existing-note async publication
  may stage native files only after preflighting the whole local manifest and
  the base ETag. Update Markdown only after ranged read-back verifies every
  file. Native-client changes to a previously published attachment are
  conflicts, never overwrites. Retry/duplicate saves must reuse verified
  receipts or deterministic staged paths, not proliferate copies.
- MediaSource/MediaStream handle background preflight and publishing. Never
  restore whole-object local `QByteArray` materialization in the Nextcloud
  media publisher. Hash/verify in bounded chunks; check the source against
  the preflight SHA-256 before uploading, and compare remote Range bytes
  against the authenticated local source before writing Markdown.
- Initialize LocalMediaStore's keychain-backed key on the application
  thread; only file reads/hashing and network transfers run on the worker.
  Preserve fail-closed behavior if external source fingerprints change.
- Never use Notes 5.0.2's attachment v1.4 upload layout for new publications:
  files are siblings of the note and break on category moves (live-tested).
  Instead create the web-native `.attachments.<noteId>/` WebDAV collection.
  Only move notes with move-safe relative links; block unknown/legacy links
  until there is a tested rehome transaction. Preserve canonical manifests
  after metadata-only moves, even if the API includes the Markdown body.
- Failed updates must leave the durable draft intact. Staged, unreferenced
  files are candidates for future reference-aware garbage collection, never
  grounds for deleting source data.
- Async load recognizes both web-native `.attachments.<noteId>/` and legacy
  flat Notes API relative files, but must not import other notes' attachment
  directories, traversal URLs, or links to Markdown documents.
  Unknown/oversized links remain intact. Bounded WebDAV ranges and capped
  response buffers are essential: never silently materialize large media in RAM.
- Media new-note publication uses a two-phase placeholder transaction:
  `anykeep.publicationDraftId` comes from encrypted DraftRecord.id, while
  direct `createNote()` provides a session token. Store an account-scoped
  attempt in QSettings **before** a Notes POST, including no draft body or
  credentials. Never repeat an ambiguous POST. Search metadata using the
  temporary heading token, GET candidate content and verify the marker
  before adopting the server ID. Retain the journal remote ID through
  completion to prevent duplicate posts across lost ACK/restart.
  Serialize claims with QLockFile (no time-based stale-lock stealing:
  a slow POST may exceed 30 seconds) and treat lock contention as a
  publication failure rather than risking a duplicate POST.
  The placeholder body must be an exact match before committing; retaining
  only its hidden marker is insufficient. Native favorite or read-only
  changes are conflicts too.
- An unresolved creation attempt must fail closed: preserve the encrypted
  draft and wait for a confirmed remote identity, without deleting any
  server note or issuing duplicate POST. If a final PUT ACK is lost,
  verify the deterministic native body and remote bytes to conclude success.
  Changed remote Markdown is a conflict, not a reason to overwrite.
- Synchronous media saves are still rejected; media capability stays disabled.
  Missing zero-byte/large remote hydration, multi-device checks and safe
  cleanup are separate lifecycle work.
- Enforce HTTPS for real Nextcloud accounts; HTTP is for loopback fixtures only.
- Do not enable `supportsMedia()` before implementing and live-testing the
  native attachment/WebDAV adapter, cross-client Markdown links, category
  moves, media durability, and cleanup. Preserve capability fallback on older
  Notes installations.

## Verification

```sh
cmake --build build/Desktop-Debug --target nextcloudcategory_test nextcloudworker_test nextcloudstorage_test -j4
ctest --test-dir build/Desktop-Debug -R '^nextcloud(category|worker|storage)_test$' --output-on-failure
```

`nextcloudworker_test` and `nextcloudstorage_test` use local HTTP fixtures and
need an environment where loopback listen is allowed.
