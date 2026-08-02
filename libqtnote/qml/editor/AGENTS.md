# Structured editor architecture

## Ownership boundaries

- `NoteEditorPaneImpl.qml`: composes toolbar, find bar, and document editor.
- `EditorToolbarImpl.qml` / `EditorActionControllerImpl.qml`: user commands and
  toolbar presentation. Commands remain shared between desktop and mobile.
- `NoteBlockEditorImpl.qml`: document-wide coordination only: selection across
  blocks, clipboard/history transactions, focus restoration, navigation, and
  block factory registration.
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
- Media blocks use `qtnote-media:` references and the shared encrypted media
  store. Image, audio, and attachment delegates must not create a parallel
  persistence path.
- A focused text delegate may defer applying `sourceText`; history restoration
  must flush or apply that pending value before restoring cursor state.
- Root-level facade files remain intentionally tiny and preserve compatibility
  for desktop resource URLs and the mobile QML module.

## Focused verification

```sh
cmake --build build/qt6_Debug --target noteblockmodel_test noteeditor_test -j4
QT_QPA_PLATFORM=offscreen ./build/qt6_Debug/tests/noteblockmodel_test -v1
QT_QPA_PLATFORM=offscreen ./build/qt6_Debug/tests/noteeditor_test -v1
```
