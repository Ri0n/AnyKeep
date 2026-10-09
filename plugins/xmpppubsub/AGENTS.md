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

## XMPP QML theme semantics

The Android application supports system/light/dark modes through its
`mobile.color-scheme` preference, `Material.theme`, and the application
`QPalette`. For XMPP settings, recovery, and trust QML, use
`palette.text` for primary information and `palette.placeholderText` for
secondary labels and explanatory copy; `palette.mid` is a **border color**
and must not be used for text. Do not dim entire unavailable-key cards: the
disabled radio button already indicates that the key cannot be selected,
while its fingerprint, device status, and explanation must remain readable.
Palette roles respond to theme changes without hardcoded light/dark values.

## First-install XMPP onboarding

When no local XMPP key exists, the recovery wizard starts with a choice:
**Use existing notes** (the standard OMEMO device/key recovery path) or
**Start with a new key**. The latter requires explicit confirmation explaining
that notes already stored in XMPP cannot be decrypted with the new key.
This path generates a local key without asking other devices for keys and
does **not** invoke the remote rekey/publish operation. The key is installed
only after the user accepts the review and finishes the wizard.

Remote index auditing is deliberately skipped for first-time setup. An
unknown count of old encrypted notes must never be displayed as zero. The
review/result screens explain that existing notes, if any, remain untouched;
users should export/save the new recovery key after setup. Back from review
abandons the uninstalled key and returns to the welcome choice. Users with
an existing local key still use the original recovery/repair flow.
This avoids bypassing key recovery silently, while permitting a new user
to initialize XMPP notes without owning another AnyKeep device.

## Verification

```sh
cmake --build build/Desktop-Debug --target xmppnotecodec_test privatenotespubsubitem_test xmppkeyresolutioncontroller_test xmppkeysyncextension_test xmppomemopubsubitems_test xmpperror_test xmpppersistenttruststorage_test xmppomemostorage_test -j4
ctest --test-dir build/Desktop-Debug -R '^(xmpp.*|privatenotespubsubitem)_test$' --output-on-failure
```

QXmpp/OMEMO targets are conditional. `xmppstorage_test` exercises the
backend-neutral storage adapter with a fake backend, but there is still no
direct `XmppWorker` integration harness. Worker/backend changes therefore
require the full suite plus explicit review of offline cache, retry, generation,
partial publication, and shutdown paths.
