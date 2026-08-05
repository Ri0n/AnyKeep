# Shared core map

`libanykeep` is the shared desktop/mobile core. Public headers in this directory
are stable include paths; implementation subdirectories may be introduced
without moving those headers.

## Behavior owners

| Domain | Primary owners | Focused tests |
| --- | --- | --- |
| Structured document/model | `noteblockmodel.h`, `noteblockmodel/`, `notefragment.*` | `noteblockmodel_test`, `notefragment_test` |
| Editor operations/history | `noteeditor*`, `editor/`, `notedocumenthistory.*`, `editorcursorcontroller.*` | `noteeditor_test`, CTest label `editor` |
| Editor QML | `qml/editor/`; read its scoped `AGENTS.md` | `editorqml_test`, `desktopnoteeditorhost_test` |
| Note lifecycle/drafts | `note.*`, `draftmanager.h`, `drafts/`, `draftstore.h`, `filedraftstore.*`, `conflictresolver.*` | `filedraftstore_test`, `draftmanagertransfer_test` |
| Workspace/transfer | `notesworkspacecontroller.h`, `workspace/`, `notetransfercontroller.*` | `notesworkspacefolders_test`, `notetransfercontroller_test` |
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

### `NoteBlockModel` implementation

| Change | Implementation owner |
| --- | --- |
| Qt model API, state/load, common normalization | `noteblockmodel/core.cpp` |
| Find results and match addressing | `noteblockmodel/search.cpp` |
| List items, subtree moves, indentation and list coalescing | `noteblockmodel/lists.cpp` |
| Tables, images, audio and attachments | `noteblockmodel/tables_media.cpp` |
| Text/heading/quote/code/tag-line and structural block moves | `noteblockmodel/structure.cpp` |
| `NoteFragment` extraction, removal, insertion and replacement | `noteblockmodel/fragments.cpp` |
| Markdown parser/writer and storage codec helpers | `noteblockmodel/markdown.cpp` |

`noteblockmodel/private.h` contains only helpers shared by multiple model
translation units. Keep domain-local helpers in their owning `.cpp`.

### `NoteEditor` operations implementation

| Change | Implementation owner |
| --- | --- |
| QTextDocument links, inline formatting and Markdown serialization | `editor/inline_formatting.cpp` |
| Clipboard export and selection ownership | `editor/clipboard.cpp` |
| Structured delete, paste and fragment/media import | `editor/fragment_transfer.cpp` |

`editor/private.h` exposes only the range helpers shared by these translation
units. Keep clipboard- or formatting-only helpers in their owning `.cpp`.

### `NotesWorkspaceController` implementation

| Change | Implementation owner |
| --- | --- |
| Construction, models, properties, editor session and operation state | `workspace/core.cpp` |
| Open/create/save/close/delete/recycle and standalone notes | `workspace/notes.cpp` |
| Move/copy/batch/reorder and staged transfer | `workspace/transfer.cpp` |
| Folder CRUD, flags, trash/undo and note assignments | `workspace/folders.cpp` |

### `DraftManager` implementation

| Change | Implementation owner |
| --- | --- |
| Store initialization, construction and recovery | `drafts/core.cpp` |
| Editing sessions, ready/discard state and queued removals | `drafts/sessions.cpp` |
| Cross-storage staging and publication retargeting | `drafts/transfer.cpp` |
| Publish/retry/conflict pipeline and storage lifetime | `drafts/publication.cpp` |

`drafts/private.h` contains only logging and diagnostic helpers shared by
multiple draft-manager translation units.

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
