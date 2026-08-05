# Shared core map

`libanykeep` is the shared desktop/mobile core. Public headers in this directory
are stable include paths; implementation subdirectories may be introduced
without moving those headers.

## Behavior owners

| Domain | Primary owners | Focused tests |
| --- | --- | --- |
| Structured document/model | `noteblockmodel.*`, `notefragment.*` | `noteblockmodel_test`, `notefragment_test` |
| Editor operations/history | `noteeditor*`, `notedocumenthistory.*`, `editorcursorcontroller.*` | `noteeditor_test`, CTest label `editor` |
| Editor QML | `qml/editor/`; read its scoped `AGENTS.md` | `editorqml_test`, `desktopnoteeditorhost_test` |
| Note lifecycle/drafts | `note.*`, `draftmanager.*`, `draftstore.h`, `filedraftstore.*`, `conflictresolver.*` | `filedraftstore_test`, `draftmanagertransfer_test` |
| Workspace/transfer | `notesworkspacecontroller.*`, `notetransfercontroller.*` | `notesworkspacefolders_test`, `notetransfercontroller_test` |
| Folders/rules | `foldercatalog*`, `foldernotesmodel.*`, `folderoperationscontroller.*`, `noterule*`, `rulescontroller.*` | matching `folder*`, `noterule*`, and `rules*` targets |
| Storage/cache/media | `notestorage.*`, `ptfstorage.*`, `filestorage.*`, `fileremotecachestore.*`, `localmediastore.*` | `ptfstorage_test`, `fileremotecachestore_test`, `localmediastore_test` |
| Plugin hosting | `pluginhost*`, `pluginmanager.*`, `pluginmetadata.*`, `bundledpluginregistry.*` | `pluginmetadata_test` plus plugin-specific tests |
| Desktop host | `anykeep.*`, `desktopnoteeditorhost.*`, `desktopnoteactions.*`, window/widget classes | `desktopnoteeditorhost_test` and relevant QML tests |

## Boundaries

- Keep `NoteBlockModel` canonical; mutations must not bypass it through QML mirrors.
- Keep public headers, Qt properties/signals, and storage/plugin interfaces stable
  during implementation-only splits.
- Shared core must not depend on desktop widgets, DBus, or a concrete plugin
  where mobile uses the same behavior. Desktop adapters may depend on shared core.
- Storage implementations own persistence; workspace/draft controllers own
  lifecycle and orchestration, not backend wire formats.
- Plugin-specific protocol and configuration code belongs under `plugins/`.

## Architecture references

Use `docs/note-editor-architecture.md` and `docs/editor-transfer-architecture.md`
for editor/model changes; use `docs/note-lifecycle-architecture.md` and
`docs/media-storage-architecture.md` for drafts, storage, and media changes.

## Verification

Build the narrow target first. For shared editor/model work use:

```sh
cmake --build build/Desktop-Debug --target editor_tests -j4
ctest --test-dir build/Desktop-Debug -L editor --output-on-failure
```

For a cross-domain core change, build `anykeep_all_tests` and run unfiltered CTest.
