# Plugin contributor map

Plugins implement optional integrations behind interfaces from `plugins/` and
`libanykeep`. Desktop plugins are dynamically discovered; the mobile allow-list
builds selected plugins as bundled static libraries.

## Isolation rules

- Keep provider protocols, DTOs, settings, and retry policy inside their plugin.
- Depend on `PluginHostInterface`, `NoteStorage`, and provider interfaces; do not
  add concrete plugin dependencies to `libanykeep`.
- A bundled plugin and its desktop counterpart must share the same implementation
  and metadata semantics. Do not fork behavior for mobile in a second backend.
- Preserve asynchronous ownership: data retained across queued calls/coroutine
  suspension is passed or captured by value, and shutdown cancels stale work.
- Never weaken TLS/certificate validation or log credentials, plaintext keys,
  access tokens, note bodies, or decrypted media.

## Plugin routing

| Plugin | Main entry point | Settings owner | Focused tests |
| --- | --- | --- | --- |
| `baseintegration` | `baseintegration.*` | none | `pluginmetadata_test` |
| `cinnamon` | `cinnamon.*` | none | `pluginmetadata_test` |
| `gemini` | `geminiplugin.*` | controller in `geminiplugin.cpp`, shared `SettingsForm.qml` | `pluginmetadata_test` |
| `gnome` | `gnome.*` | none | `pluginmetadata_test` |
| `kdeintegration` | `kdeintegration.*` | controller in `kdeintegration.cpp`, shared form | `pluginmetadata_test` |
| `macosx` | `macosx.*` | none | `pluginmetadata_test` |
| `nextcloud` | `nextcloudplugin.*` | `nextcloudstorage.cpp`, shared form | `nextcloudcategory_test`, `nextcloudworker_test`, `nextcloudstorage_test` |
| `openaiwhisper` | `openaiwhisperplugin.*` | controller in entry point, shared form | `pluginmetadata_test` |
| `spellchecker` | `spellcheckplugin.*` | `SpellcheckSettings.qml` and controller in entry point | `settingsqml_test`, `pluginmetadata_test` |
| `tomboy` | `tomboyplugin.*` | none | `tomboyfolderoverlay_test`, `pluginmetadata_test` |
| `yandex` | `yandexplugin.*` | `YandexSettings.qml`, `yandexsettingscontroller.*` | `yandexspeechutils_test` when Multimedia is available |
| `xmpppubsub` | `xmppplugin.*` | `XmppSettings.qml`, `xmppsettingscontroller.*` | read `xmpppubsub/AGENTS.md` |

Read the plugin's scoped map before touching Nextcloud or XMPP. Test target
availability follows optional Qt/provider dependencies in CMake.

## XMPP connection indicators across tray implementations

The default and macOS system-tray implementations call
`TrayIconUtils::setupSystemTrayIcon`, which subscribes to
`NoteManager::storageChanged` and projects the storage's transport
connectivity into the tray icon badge, tooltip and read-only menu action.
A local cache that remains accessible offline must never produce a green
online indicator.

KDE's Plasma applet is a **separate process**, not a `QSystemTrayIcon`.
`AnyKeepDBus::storageConnectivityJson` returns the same provider-neutral
state with a user-readable summary; `storageConnectivityChanged` prompts
the applet to refresh without polling. The Plasmoid model clears status on
service exit and ignores stale asynchronous replies. The compact applet
overlays the matching badge and includes the status in the tooltip.

Do not import the XMPP plugin types into the tray/plasmoid. Treat
`NoteStorage::ConnectivityState::NotApplicable` as "no indicator" for
local storages, not as a disconnected remote account.
