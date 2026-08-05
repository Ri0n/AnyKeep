# Structured editor architecture

## Ownership boundaries

- `NoteEditorPaneImpl.qml`: composes toolbar, find bar, and document editor.
- `EditorToolbarImpl.qml` / `EditorActionControllerImpl.qml`: user commands and
  toolbar presentation. Commands remain shared between desktop and mobile.
- `NoteBlockEditorImpl.qml`: composition, visual editor infrastructure, and the
  stable forwarding API used by delegates, toolbar, backend, and tests.
- `controllers/EditorSelectionController.qml`: document selection,
  clipboard/history transactions, structured paste, cut, and deletion.
- `controllers/EditorFocusController.qml`: editor registry/addressing,
  focus/state restoration, viewport preservation, and text insertion targets.
- `controllers/EditorMediaNavigationController.qml`: image/audio/attachment
  selection, removal, and navigation across structural boundaries.
- `components/NoteBlockTextArea.qml`: the common editable text surface, including source
  synchronization, formatting, spell checking, link interaction, and local
  pointer/keyboard handling.
- `EditorBlockDelegate.qml`: visual shell for one model row and the structural
  reorder handle.
- `EditorBlockFactories.qml`: creates the reusable `Component` factories once
  per editor instance and supplies them to delegates. Do not create a full
  factory set per delegate.
- `blocks/`: one component per structural block type. A block component may
  call the public coordination API exposed by `editorView`, but must not own
  document-wide selection or history state.
- `InterBlockHitLayer.qml` / `TrailingDocumentArea.qml`: blank-boundary pointer
  handling.
- `support/EditorMarkdownRendering.js`: pure Markdown-to-QTextDocument rendering
  transformations.
- `EditorReorderController.qml`, `components/ListBlockEditor.qml`, and `components/TagLineEditor.qml`:
  editor adapters over the generic primitives in `../reorder/`.

## Invariants

- `NoteBlockModel` is canonical. QML delegates may mirror roles for editing or
  animation but commit one model mutation per user operation.
- Public methods used by toolbar, history, tests, or block components stay on
  `NoteBlockEditorImpl`; implementation files must not reach into another
  block delegate by private id.
- Media blocks use `anykeep-media:` references and the shared encrypted media
  store. Image, audio, and attachment delegates must not create a parallel
  persistence path.
- A focused text delegate may defer applying `sourceText`; history restoration
  must flush or apply that pending value before restoring cursor state.
- Root-level facade files remain intentionally tiny and preserve compatibility
  for desktop resource URLs and the mobile QML module.

## `NoteBlockEditorImpl` public API owners

| API group | Implementation owner |
| --- | --- |
| `registerEditor`, addresses, capture/restore, `focus*`, speech target | `EditorFocusController.qml` |
| selection ranges, transactions, copy/cut/paste, structured delete | `EditorSelectionController.qml` |
| `select*Block`, `remove*Block`, adjacent/boundary navigation | `EditorMediaNavigationController.qml` |
| insert/convert/inline formatting, menus, block factories and layout | `NoteBlockEditorImpl.qml` |

Keep existing public properties as aliases and public methods as short
forwarders when implementation moves. Controllers receive the editor view,
model, and backend explicitly; they must not reach into private visual ids.

## Focused verification

```sh
cmake --build build/Desktop-Debug --target editor_tests -j4
ctest --test-dir build/Desktop-Debug -L editor --output-on-failure
```
