# Android build

AnyKeep has a Qt Quick Android target. An Android kit enables
`ANYKEEP_BUILD_MOBILE` automatically; desktop builds continue to use the desktop
application and the shared Qt Quick editor.

## Supported baseline

- Qt for Android: **Qt 6.11 or newer**;
- minimum Android version: **Android 9 / API 28**;
- primary release ABI: **arm64-v8a**;
- desktop and shared sources that are also compiled by the desktop target remain
  compatible with Qt 6.4 until the desktop baseline is changed separately.

The Android target is allowed to use Qt 6.11 APIs because Qt is deployed with the
APK/AAB. `QT_ANDROID_MIN_SDK_VERSION` is set to 28 on `anykeep_mobile`.

## Qt Creator setup

1. Install Qt 6.11 for Desktop and Android, including the `arm64-v8a` Android
   architecture and Qt Quick Controls.
2. In **Preferences > SDKs > Android**, configure a 64-bit JDK and let Qt
   Creator install the SDK, NDK and build tools required by that Qt version.
3. Open the repository's root `CMakeLists.txt` and select the generated Android
   `arm64-v8a` kit.
4. Configure the project. `ANYKEEP_BUILD_MOBILE` is enabled automatically when
   the Android toolchain sets `ANDROID`; the build target is `anykeep_mobile`.
5. Select an emulator or a device with USB debugging enabled, then Build and
   Run. Qt Creator invokes `androiddeployqt` and packages the target as an APK.

A non-Android kit may still build `anykeep_mobile` as a QML-shell preview by
passing `-DANYKEEP_BUILD_MOBILE=ON`. Android-only services are disabled in that
configuration.

## Current boundary

Android and desktop share `NotesModel`, `NotesSearchModel`, `RecentNotesModel`,
`NotesWorkspaceController`, `PluginListModel`, `StoragePriorityModel`,
`NoteEditor`, the structured QML editor, find bar, adaptive toolbar, dialog
service and settings controllers. Android opens in the flat Recent view;
desktop defaults to the storage-grouped tree.

PTF is registered through the same core startup function on both platforms.
Android plugin discovery uses the explicit bundled factory registry documented
in [Android bundled plugin loading](mobile-plugin-loading.md). Nextcloud is
always included. Android builds QCoro 0.13, QXmpp with OMEMO, libomemo-c 0.5.1
and the protobuf-c runtime from source by default when suitable target packages
are unavailable. QCA and QXmpp share the same prebuilt Android OpenSSL bundle
already used by the application; AnyKeep does not compile a second OpenSSL copy.
The native dependency order is:

```text
Android OpenSSL -> QCA
Android OpenSSL -> QXmpp + QXmpp OMEMO -> AnyKeep XMPP plugin
protobuf-c runtime -> libomemo-c ---------^
```

The source-built native libraries use the active Android toolchain and ABI.
OpenSSL is selected from `<Android SDK>/android_openssl/ssl_3/<ABI>` or from
`ANYKEEP_ANDROID_OPENSSL_ROOT` when the bundle is installed elsewhere.
XMPP Private Notes is included when the resulting build exposes
`QXmpp::QXmpp`, `QXmpp::Omemo` and `QCoro::Core`; otherwise it is omitted at
configure time. For offline builds, point `ANYKEEP_QCORO_SOURCE_DIR`,
`ANYKEEP_QXMPP_SOURCE_DIR`, `ANYKEEP_OMEMO_C_SOURCE_DIR` and
`ANYKEEP_PROTOBUF_C_SOURCE_DIR` to local source trees. The individual fallbacks
can be disabled with `ANYKEEP_BUILD_BUNDLED_QCORO=OFF`,
`ANYKEEP_BUILD_BUNDLED_QXMPP=OFF` or `ANYKEEP_BUILD_BUNDLED_OMEMO_C=OFF`.
`QXmppOmemoQt6_DIR-NOTFOUND` may remain in CMakeCache when no system package is
installed; it does not disable the bundled target chain. Gemini speech and
OpenAI Whisper remain desktop plugins and are not linked into the Android
application.

Android platform services currently provide:

- system Share chooser for note text;
- system document picker for exporting `.txt` or `.md`;
- opt-in Android speech recognition with runtime microphone permission;
- pinned launcher shortcuts that open a specific persisted note.

There is no separate Android PrintManager integration. Printing, when offered by
an installed application or print service, is reached through the system Share
flow. There is also no manual Save action: editing checkpoints are automatic;
Share and Export are explicit external-output operations.

## Remaining hardening

- physical-device IME, predictive-input and speech-service tests;
- background, process-death and draft recovery tests;
- rotation and selection restoration;
- launcher shortcut behavior for an already running activity;
- Share/content-URI compatibility across common applications;
- bundled crypto/native-library verification;
- arm64 release builds, signing, AAB metadata and store validation.
