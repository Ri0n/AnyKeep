# Mobile application shell map

This directory owns the Qt Quick/Android application shell. Shared models,
storage orchestration, and reusable editor/manager QML remain in `libanykeep`.

## Ownership

| Area | Owner |
| --- | --- |
| Process/application composition | `main.cpp`, `mobileapplication.*` |
| Root navigation and pages | `Main.qml`, `RootPage.qml`, page QML files |
| Mobile editor chrome | `NoteEditorPage.qml`, `MobileNoteTopBar.qml`, `MobileNoteActionBar.qml` |
| Android intents, permissions, sharing/services | `androidplatformservices.*` |
| Mobile editor platform adapter | `mobileeditorplatformbackend.*` |
| Bundled plugin registration | `mobilebundledplugins.*`, `plugins/CMakeLists.txt` allow-list |
| Shared QML resource aliases | `src/mobile/CMakeLists.txt` |

## Boundaries

- Do not copy shared QML into this directory. Add it to `libanykeep/qml/` and
  preserve its resource alias in `src/mobile/CMakeLists.txt`.
- Keep Android APIs behind `AndroidPlatformServices`; shared core and QML must
  remain usable by the desktop host.
- Mobile plugins use the explicit bundled allow-list and the same implementation
  as desktop plugins. Registration code must not contain provider behavior.
- Keep navigation/page state in the shell; document, folder, draft, and storage
  state belongs to shared controllers.

## Verification

For shared editor/manager QML, run the corresponding desktop headless tests
first (`editor` label or `notesmanagerqml_test`). For shell/C++ changes, build
the configured `anykeep_mobile` target; Android packaging/device checks remain
separate from the desktop CTest suite.
