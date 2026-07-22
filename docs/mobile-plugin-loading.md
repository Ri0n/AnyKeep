# Android bundled plugin loading

## Decision

Android uses an explicit factory registry for plugins compiled into the APK.
It does not use desktop shared-library discovery and does not use
`Q_IMPORT_PLUGIN` as the application-level contract.

The shared pieces are:

- `PluginListSource`: UI-neutral source contract used by `PluginListModel`;
- `PluginManager`: desktop dynamic-plugin implementation;
- `BundledPluginRegistry`: Android/bundled implementation;
- `BundledPluginInterface`: UI-neutral bundled lifecycle;
- `registerMobileBundledPlugins()`: explicit allow-list and factory table.

```mermaid
flowchart LR
    QML[PluginsPage.qml] --> MODEL[PluginListModel]
    MODEL --> SOURCE[PluginListSource]
    SOURCE --> DESKTOP[PluginManager]
    SOURCE --> MOBILE[BundledPluginRegistry]
    MOBILE --> TABLE[registerMobileBundledPlugins]
    TABLE --> FACTORY[Factory]
    FACTORY --> RUNTIME[BundledPluginInterface]
```

## Shared lifecycle

`RegularPluginInterface/2.0` no longer accepts the desktop `Main` shell. Dynamic
plugins implement `initialize()/shutdown()` after receiving
`PluginHostInterface`. A runtime that is also Android-compatible may implement
`BundledPluginInterface` with the same methods. Storage registration uses the
shared `NoteManager`; settings use `SettingsController` plus QML.

When a plugin source is compiled directly into `qtnote_mobile`,
`QTNOTE_BUNDLED_PLUGIN_BUILD` suppresses `Q_PLUGIN_METADATA`. This prevents
multiple bundled classes from exporting the same dynamic-plugin entry symbols.
Desktop plugin libraries retain normal metadata.

## Current Android allow-list

- Nextcloud Notes storage.

PTF remains a core storage and is registered through `registerCoreStorages()`
rather than the plugin registry.

Gemini and OpenAI Whisper are intentionally not linked into Android. Android
voice input is an opt-in platform service, enabled in application settings and
exposed by the shared toolbar only while enabled. The desktop speech-provider
plugin contract remains available; a future Android-capable provider can be
admitted explicitly without changing the platform fallback.

## Not yet admitted

- Hunspell: Android dictionary packaging/download paths need device validation;
- XMPP: QXmpp/QCoro/OMEMO packaging and the remaining key-conflict recovery
  wizard must be made QML-only;
- desktop integration, tray, global-shortcut and notification plugins: these are
  desktop services rather than Android application plugins;
- Tomboy: its backend and file-format assumptions require Android storage-access
  review.

Registration is explicit. Merely listing `android` in a plugin CMake declaration
does not put the runtime into the APK.
