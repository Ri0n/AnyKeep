# AnyKeep contributor map

- `libanykeep/`: shared C++ models, storage, controllers, and desktop host API.
- `libanykeep/qml/`: shared desktop/mobile QML; read its scoped `AGENTS.md`.
- `libanykeep/qml/editor/`: structured editor internals; read its scoped `AGENTS.md`.
- `src/` and `src/mobile/`: desktop and Android application shells.
- `plugins/`: optional integrations; keep plugin-specific code out of `libanykeep`.
- `tests/`: QtTest executables; read `tests/AGENTS.md` before changing tests.

Architecture references (use the scoped `AGENTS.md` first, then open only the
document relevant to the change):

- note architecture index: `docs/note-architecture.md`;
- shared live model/lifetime: `docs/note-live-model.md`;
- transfer/publication: `docs/note-transfer-publication.md`;
- recovery/durability: `docs/note-recovery.md`;
- editor composition: `docs/note-editor-architecture.md`;
- undo/history: `docs/note-editor-undo-redo.md`;
- structured clipboard and media transfer: `docs/editor-transfer-architecture.md`;
- lifecycle summary/index: `docs/note-lifecycle-architecture.md`;
- local/remote media: `docs/media-storage-architecture.md`;
- proposed shared chunk scheduler/download pipeline: `docs/media-range-pipeline.md`;
- manager and folders UI: `docs/notes-manager-architecture.md`.

Use the existing `build/Desktop-Debug` tree. Prefer focused targets and CTest labels:

```sh
cmake --build build/Desktop-Debug --target editor_tests -j4
ctest --test-dir build/Desktop-Debug -L editor --output-on-failure
```

Use target `anykeep_all_tests` followed by unfiltered CTest for a full run.

CTest supplies the offscreen/software-rendering environment. Run executables
directly only when test arguments or verbose diagnostics are needed, and then
set `QT_QPA_PLATFORM=offscreen` and `QSG_RHI_BACKEND=software`.

Keep `NoteBlockModel` canonical, preserve the tiny root QML compatibility
facades, and do not mix desktop-only dependencies into the shared mobile core.
Preserve unrelated working-tree changes and verify with `git diff --check`.

## Model/view stability (mandatory)

Model mutations must have the smallest possible impact on an attached view.
Preserve unaffected row identities, persistent indexes, delegates, focus,
selection, scroll position, and active editors. For value/role changes emit
`dataChanged` for the exact indexes and roles; for structural changes use
precise `beginInsertRows` / `beginRemoveRows` / `beginMoveRows` and their
matching end calls. A local change must not rebuild an entire storage, tree,
list, or proxy model. Treat `beginResetModel` / `endResetModel` as exceptional,
for a genuinely new data source or unreconcilable structural replacement,
not as the normal update mechanism.

Prevent flicker, jumpiness, disappearing selection, and delegate churn at the
source. **Do not paper over a model/view glitch** with `Timer`, `Qt.callLater`,
post-update scrolling/focus restoration, extra model resets, or similar
"repair after the fact" workarounds. Deferral is permissible for real
asynchronous operations and lifecycle ordering, never as a substitute for
correct model notifications and stable identities.

Before changing model-update code, check its effect on attached views,
especially Qt Quick `TreeView`, `ListView`, editing delegates, drag-and-drop,
and pagination. Add regressions that verify unaffected rows and view state
remain stable. If a correct minimal-impact solution requires a substantial
architectural rewrite, postpone it as a separately scoped task rather than
shipping a fragile workaround.

## Commit messages and squash merges

Write commit messages in English. Use a concise imperative subject describing
the resulting behavior. For a squash commit, describe the complete final change
as one coherent unit, including relevant fixes made during review.

For a nontrivial change, include a body explaining the problem, what changed and
why, plus the validation performed and any material limitations. Scale the
detail to the change; do not dump implementation details or conversational
history into the message.

Keep the PR title and description aligned with the final implementation so
they can serve as the squash commit subject and body. When performing a squash
merge or rewriting a commit, explicitly supply the final subject and body.
Do not use an automatically concatenated list of intermediate commit messages,
repeated fixup subjects, or abandoned approaches as the squash commit body.
