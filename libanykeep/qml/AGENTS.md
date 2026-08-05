# Shared QML map

## Editor entry points

The root-level editor files are compatibility facades. Keep their public type
names and properties stable because desktop hosts, tests, and the mobile QML
module instantiate them directly.

- `NoteEditorPane.qml` -> `editor/NoteEditorPaneImpl.qml`
- `NoteBlockEditor.qml` -> `editor/NoteBlockEditorImpl.qml`
- `EditorToolbar.qml` -> `editor/EditorToolbarImpl.qml`
- `EditorActionController.qml` -> `editor/EditorActionControllerImpl.qml`
- `NotesManagerPage.qml` -> `notesmanager/NotesManagerPageImpl.qml`

Read `editor/AGENTS.md` before changing editor internals.

## Other shared areas

- `notelist/`: note-list rows, selection, and collection views.
- `notesmanager/`: manager page implementation and action/drag controllers;
  read its scoped `AGENTS.md`.
- `reorder/`: generic reorder primitives shared by the editor, folders,
  settings, rules, and note lists. Do not move these under `editor/`.
- `ThemedIcon.qml`, `DialogHost.qml`, and `FolderPickerMenu.qml` are shared UI
  primitives rather than editor implementation details.
