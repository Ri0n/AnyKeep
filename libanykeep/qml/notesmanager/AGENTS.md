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

## Model/view update discipline

Keep grouped, folder, and Recent views attached to stable item identities.
Single-note changes (including draft state, recycle status, favorite, loading,
and title) must not clear and repopulate the entire note collection. Prefer
precise source-model notifications and incremental proxy updates over resets
and mass remove/reinsert cycles. Preserve `TreeView` delegates, expansion,
selection, editing focus, and scroll anchors for unaffected rows.

Never use delayed re-selection, `Qt.callLater` scrolling, or timers to hide
a glitch caused by an excessive model update. Fix the model mutation that
causes the view churn. Postpone large refactors if they cannot be completed
correctly and independently.

## Verification

```sh
cmake --build build/Desktop-Debug --target notesmanagerqml_test notesworkspacefolders_test -j4
ctest --test-dir build/Desktop-Debug -R '^(notesmanagerqml_test|notesworkspacefolders_test)$' --output-on-failure
```
