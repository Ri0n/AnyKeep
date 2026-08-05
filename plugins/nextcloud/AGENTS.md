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

## Verification

```sh
cmake --build build/Desktop-Debug --target nextcloudcategory_test nextcloudworker_test nextcloudstorage_test -j4
ctest --test-dir build/Desktop-Debug -R '^nextcloud(category|worker|storage)_test$' --output-on-failure
```

`nextcloudworker_test` and `nextcloudstorage_test` use local HTTP fixtures and
need an environment where loopback listen is allowed.
