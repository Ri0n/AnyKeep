# Android build

QtNote has a Qt Quick Android target. An Android kit enables
`QTNOTE_BUILD_MOBILE` automatically; desktop builds continue to use the desktop
application and the shared Qt Quick editor.

## Supported baseline

- Qt for Android: **Qt 6.11 or newer**;
- minimum Android version: **Android 9 / API 28**;
- primary release ABI: **arm64-v8a**;
- desktop and shared sources that are also compiled by the desktop target remain
  compatible with Qt 6.4 until the desktop baseline is changed separately.

The Android target is allowed to use Qt 6.11 APIs because Qt is deployed with the
APK/AAB. `QT_ANDROID_MIN_SDK_VERSION` is set to 28 on `qtnote_mobile`.

## Qt Creator setup

1. Install Qt 6.11 for Desktop and Android, including the `arm64-v8a` Android
   architecture and Qt Quick Controls.
2. In **Preferences > SDKs > Android**, configure a 64-bit JDK and let Qt
   Creator install the SDK, NDK and build tools required by that Qt version.
3. Open the repository's root `CMakeLists.txt` and select the generated Android
   `arm64-v8a` kit.
4. Configure the project. `QTNOTE_BUILD_MOBILE` is enabled automatically when
   the Android toolchain sets `ANDROID`; the build target is `qtnote_mobile`.
5. Select an emulator or a device with USB debugging enabled, then Build and
   Run. Qt Creator invokes `androiddeployqt` and packages the target as an APK.

A non-Android kit may still build `qtnote_mobile` as a QML-shell preview by
passing `-DQTNOTE_BUILD_MOBILE=ON`. Android-only services are disabled in that
configuration.

## Current boundary

Android and desktop share `NotesModel`, `NotesSearchModel`, `RecentNotesModel`,
`NotesWorkspaceController`, `PluginListModel`, `StoragePriorityModel`,
`NoteEditor`, the structured QML editor, find bar, adaptive toolbar, dialog
service and settings controllers. Android opens in the flat Recent view;
desktop defaults to the storage-grouped tree.

PTF is registered through the same core startup function on both platforms.
Android plugin discovery uses the explicit bundled factory registry documented
in [Android bundled plugin loading](mobile-plugin-loading.md). Nextcloud is the
current Android allow-list. Gemini speech and OpenAI Whisper remain desktop
plugins and are not linked into the Android application.

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
