# XMPP Private Notes plugin map

Read `README.md` for component/lifetime design, `PROTOXEP.md` before changing
the wire format, and `INTEROPERABILITY.md` before changing encryption/codec
behavior or reference vectors.

## Ownership

| Area | Owner |
| --- | --- |
| Plugin registration | `xmppplugin.*` |
| Storage API, cache, jobs, reconnect/retry | `xmppstorage.h`, `storage/` |
| QXmpp connection, PEP/PubSub CRUD, OMEMO flows, maintenance | `xmppworker.h`, `worker/` |
| Backend-neutral async contract and DTOs | `xmppbackend.h`, `xmppdto.h` |
| Encrypted note wire codec | `xmppnotecodec.*`, `privatenotespubsubitem.*` |
| PubSub/key-sync extensions | `xmpppepextension.*`, `xmppkeysyncextension.*` |
| OMEMO/trust persistence | `xmppomemostorage.*`, `xmpppersistenttruststorage.*` |
| Recovery/trust UI flow | `xmppkeyresolutioncontroller.*`, `xmppdialogpresenter.*`, host QML |
| Settings and maintenance UI | `XmppSettings.qml`, `xmppsettingscontroller.*` |

## Protocol and lifetime invariants

- Keep one `XmppStorage`, one backend worker, and Qt's normal event loop; do not
  add threads or nested event loops during implementation-only splits.
- Backend async arguments are values intentionally. Preserve generation checks,
  cancellation, terminal shutdown, shared preparation, and storage-owned retry.
- Preserve namespace/version, authenticated fields, fixed vectors, and
  index/content revision binding unless the protocol itself is explicitly changed.
- TLS is mandatory. Never ignore certificate errors or log passwords, storage
  keys, decrypted payloads, OMEMO secrets, or note plaintext.
- Maintenance must not delete unreadable/authentication-protected items.

## Implementation routing

| Change | Owner |
| --- | --- |
| QXmpp client lifetime, connection, OMEMO readiness and node setup | `worker/connection.cpp` |
| Backend entry points, note list/load/save/index/delete and inbound key-sync routing | `worker/notes.cpp` |
| Own-device OMEMO operations, discovery and storage-key audit | `worker/omemo.cpp` |
| Cleanup, rekey, approved key exchange and own-bundle repair | `worker/maintenance.cpp` |
| Storage construction, protected configuration, initialization and key recovery | `storage/core.cpp` |
| DTO conversion, folder paths, persistent cache and body prefetch | `storage/cache_folders.cpp` |
| Storage note list/load/save/folder/delete jobs | `storage/crud.cpp` |
| Remote events, error/retry state, config apply and settings controller | `storage/events_retry.cpp` |

`worker/private.h` owns shared QXmpp result helpers. `storage/private.h` owns
shared backend keys, retry bounds, keychain names and status conversion. Do not
move mutable worker/storage state into these headers.

## Verification

```sh
cmake --build build/Desktop-Debug --target xmppnotecodec_test privatenotespubsubitem_test xmppkeyresolutioncontroller_test xmppkeysyncextension_test xmppomemopubsubitems_test xmpperror_test xmpppersistenttruststorage_test xmppomemostorage_test -j4
ctest --test-dir build/Desktop-Debug -R '^(xmpp.*|privatenotespubsubitem)_test$' --output-on-failure
```

QXmpp/OMEMO targets are conditional. There are currently no direct
`XmppWorker`/`XmppStorage` tests, so changes to them also require the full suite
and explicit review of offline cache, retry, generation, and shutdown paths.
