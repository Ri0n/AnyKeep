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


## Release APK size

Android Release links `anykeep_mobile` with lld's `--strip-debug` option,
removing DWARF sections as part of linking rather than modifying the generated
`.so` afterward. A post-link `llvm-strip` command executed by CI was
insufficient: after stripping the 107 MB ELF down to about 8 MB, Ninja relinked
it when building Qt's `apk` target, and Qt deployed the unstripped binary.

The shared Android build action builds the `apk` target directly, then checks
that the linked application ELF has no DWARF sections and is at most 32 MiB.
It also checks that the packaged library's size matches that linked ELF, and
rejects an APK larger than 140 MiB. The release-only linker option does not
remove debug sections from Debug or RelWithDebInfo builds.

Gradle's `stripReleaseDebugSymbols` may still warn that it cannot locate the
NDK provisioned outside the SDK's side-by-side `ndk/` directory. This does
not affect the application ELF because lld has already removed its debug
sections. Most Qt, Iris, QCA and FFmpeg libraries shipped by upstream/prebuilt
packages are already stripped.

## APK signing in CI

The shared Android build action selects exactly one Release APK, aligns it with
`zipalign -P 16`, then signs it with `apksigner`. Signature and alignment checks
and the native payload audit run against the final APK in `packages/` before
artifact upload. An unsigned Gradle Release output is never published directly.

The packaging workflow accepts these GitHub repository secrets:

- `ANDROID_SIGNING_KEYSTORE_BASE64`: base64-encoded persistent keystore;
- `ANDROID_SIGNING_KEY_ALIAS`: signing key alias;
- `ANDROID_SIGNING_STORE_PASSWORD`: keystore password;
- `ANDROID_SIGNING_KEY_PASSWORD`: key password.

The packaging workflow requires all four secrets and fails if any is missing;
published APKs always use the persistent key. PR CI explicitly enables
`allow-debug-signing` and uses temporary keys without access to release signing
secrets. These test APKs have a different certificate between builds and cannot
update installations signed with the persistent key. Keystore files are removed
after signing and passwords are passed through environment variables.

## Production Qt plugin pruning

Android Release targets use `qt_import_plugins(anykeep_mobile EXCLUDE_BY_TYPE qmltooling)`
to prevent the Qt QML debugger, inspector and profiler plugins from entering
Release APKs. Debug configurations continue to allow QML debugging. Qt's platform, network/TLS, SVG/image formats, and multimedia
plugins remain under the standard deployment mechanism. This is preferable to
setting `QT_ANDROID_DEPLOYMENT_DEPENDENCIES`, which **replaces** all automatic
Qt dependency discovery and could easily omit runtime dependencies.

The CI script `.github/scripts/inspect-android-apk.py` audits each APK:
it requires no QML tooling plugins, checks the critical platform/TLS/media
and Material+Basic style plugins, compares the packaged AnyKeep ELF to the
linked ELF, and logs a native library inventory with the size baseline from
PR #142. The compiled material QML code still needs its Basic fallback.

## Material-only Android Release packaging

The Android Release CI package runs Qt's normal `qmlimportscanner` through
`.github/scripts/filter-android-qml-imports.py`. This strips **only** QML import
records belonging to the unused `QtQuick.Controls.Fusion`,
`QtQuick.Controls.Imagine`, `QtQuick.Controls.Universal` and
`QtQuick.Controls.FluentWinUI3` modules (and their `.impl` modules).
The scanner still visits the original QML source, and `androiddeployqt`
performs its ordinary native dependency resolution. Unlike manually deleting
libraries from an APK or overriding `QT_ANDROID_DEPLOYMENT_DEPENDENCIES`,
the deployment process retains control over the native startup library list.

Material is Android's default and is also imported by `src/mobile/Main.qml`.
Basic remains required as its Qt fallback; platform, SSL/TLS, SVG/image,
multimedia and third-party libraries are unaffected. The APK inventory
fails if any excluded style's native libraries are still included.

This filter is currently attached to the CI Release packaging action, not Qt
Creator Debug builds. The Android launch smoke test is still a necessary
follow-up before treating the trimmed APK as generally deployable; presence
checks alone cannot prove every dynamically loaded QML component works.

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

## Android list title visibility

The shared `NoteListRow` displays the `title` role from
`RecentNotesModel`. On Android touch layouts, its title label takes the
foreground from the **Material theme**, which is configured by the
`ApplicationWindow` in `src/mobile/Main.qml`. Desktop delegates continue
to use the generic Qt palette. An empty published title uses the same
`Untitled note` presentation fallback as a pending draft; this is a UI
fallback and does not change the stored note body/title or PTF file.

The touch list's QML regression test checks that the visible label displays
the model title and tracks a subsequent title role update. PTF storage
round-trip tests separately cover extraction of an explicit first-line title.
Device verification should include both light and dark theme and a newly
created PTF note.

## Remaining hardening

- physical-device IME, predictive-input and speech-service tests;
- background, process-death and draft recovery tests;
- rotation and selection restoration;
- launcher shortcut behavior for an already running activity;
- Share/content-URI compatibility across common applications;
- bundled crypto/native-library verification;
- arm64 release builds, signing, AAB metadata and store validation.
