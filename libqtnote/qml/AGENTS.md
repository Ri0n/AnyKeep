# Shared editor QML map

Read this file before opening `NoteBlockEditor.qml`.

## Entry points

- `NoteEditorPane.qml`: shared toolbar/editor composition used by desktop and
  Android.
- `NoteBlockEditor.qml`: document-wide selection, clipboard, focus navigation,
  block delegates, and the shared `BlockTextArea` implementation.
- `ListBlockEditor.qml`: list rows, marker geometry, item spacing, local mirror
  model, drop gaps, and keyboard delegation to `ListBlockBehavior.js`.
- `EditorReorderController.qml`: one drag state for the whole document. It
  chooses cross-list insertion boundaries and indentation, keeps dragged rows
  under the pointer, and commits one model operation on release.
- `ReorderDragHandle.qml`: pointer gesture only. It reports absolute
  `activeTranslation`; it must not mutate a document model.
- `ListBlockBehavior.js`: Enter/Backspace/Tab and list boundary keyboard rules.
- `TableBlockBehavior.js`: table keyboard rules.

## List invariants

- `NoteBlockModel` is canonical. `ListBlockEditor` mirrors roles only to create
  editable delegates; drag previews never reorder that mirror.
- A list item and all following items with a greater indent form one movable
  subtree.
- `moveListSubtree()` receives a target insertion offset after removing the
  source subtree. Cross-block target rows are the pre-operation model rows.
- Marker slots have one fixed width. Bullet, task, and numbered markers align to
  the first text line, not the full wrapped item.
- Source rows collapse and target gaps expand only visually. One atomic model
  mutation is made on release and must remain one undo step.
- Horizontal drag selects an indent from `0` through `previousIndent + 1`.
- Markdown list continuations serialize under the marker content column:
  4 spaces for `- ` and 6 spaces for `- [ ] `.

## Focused verification

```sh
cmake --build build/qt6_Debug --target noteblockmodel_test qmlnoteeditor_test -j4
QT_QPA_PLATFORM=offscreen ./build/qt6_Debug/tests/noteblockmodel_test -v1
QT_QPA_PLATFORM=offscreen ./build/qt6_Debug/tests/qmlnoteeditor_test -v1
```

For list work, search test names containing `List`, `list`, `drag`, and
`MarkdownConversion` in `tests/qmlnoteeditor_test.cpp` and
`tests/noteblockmodel_test.cpp`.
