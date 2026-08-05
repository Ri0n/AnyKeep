# Notes manager QML map

`../NotesManagerPage.qml` is the stable compatibility facade. Keep its public
type/resource URL and the properties/functions exposed by `NotesManagerPageImpl`.

## Ownership

| Area | Owner |
| --- | --- |
| Layout, view mode, search chrome, selection state, menus/dialog composition | `NotesManagerPageImpl.qml` |
| Open/create/delete/restore, folder assignment, move/copy menu commands | `NotesManagerActionController.qml` |
| Grouped/recent drag boundaries and commit routing | `NotesManagerDragController.qml` |
| Generic drag mechanics and displacement | `../reorder/` |
| Note rows, tree presentation and selection | `../notelist/` |

## Invariants

- The page owns the single selection and drag sources of truth; controllers
  receive them explicitly and must not duplicate their state.
- Controllers may receive concrete menus, dialogs, editor pane, or folder view
  as required properties. They must not resolve private ids implicitly.
- Keep business operations routed through `workspace`; QML does not mutate
  storage, drafts, or folder catalog directly.
- Keep the root facade tiny and register implementation/controller resource
  aliases for both desktop and mobile.

## Verification

```sh
cmake --build build/Desktop-Debug --target notesmanagerqml_test notesworkspacefolders_test -j4
ctest --test-dir build/Desktop-Debug -R '^(notesmanagerqml_test|notesworkspacefolders_test)$' --output-on-failure
```
