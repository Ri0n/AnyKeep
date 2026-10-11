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
  the base ETag. Update Markdown only after read-back verifies every file.
  Failed updates must leave the durable draft intact; staged uploads are
  orphans for a future cleanup pass, never grounds for deleting source data.
- Native attachments are imported by the async load job only for
  `.attachments.<noteId>/` paths. Keep unknown links untouched. Bounded
  WebDAV ranges are essential: never silently materialize a large file in RAM.
- A new note with media is still rejected: it needs a durable recoverable
  remote note ID before attachment staging. Synchronous media saves are rejected.
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
