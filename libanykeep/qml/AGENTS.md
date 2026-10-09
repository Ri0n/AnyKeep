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

## Folder creation UI and metadata

`FoldersPage.qml` is shared between desktop and Android. The `touchActions`
mode uses a modal **Create folder** dialog: collect a nonblank name, **Add to
favorites**, and **Hide from menu (Archive)** before inserting the folder.
Desktop continues to create an unnamed folder then inline-rename it. Existing
folder renaming remains inline on both platforms.

Both mobile creation entry points use the same dialog: the toolbar creates at
root; **New subfolder** supplies the selected parent ID. Cancelling or leaving
the name empty must not change the catalog. A failed create keeps the dialog
open and displays `workspace.errorString`. Successful create expands the
parent, selects the new folder and never starts inline rename.

`NotesWorkspaceController::createFolderWithFlags` persists name, parent and
both folder flags **in one encrypted catalog transaction**. Do not recreate a
create-then-`setFolderFlags` sequence: it exposes intermediate metadata to
native-provider folder synchronization. Both flags are supported; archived
branches are hidden from the navigation menu but remain in the folder tree.
The collapsed flag is display state, not an initial folder attribute.


## Storage connectivity display

`AnimatedSettingsList.qml` renders `StoragePriorityModel`'s
`connectivityState` and `connectivityText` with a small colored icon
badge and a legible subtitle. Both are suppressed in plugin mode and for
local/non-network storages (`NotApplicable`). Connectivity is **not** the
same as `accessible`: XMPP can serve cached notes while offline. Reuse
this model state instead of watching provider-specific signals in QML.
