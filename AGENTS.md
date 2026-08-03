# AnyKeep contributor map

- `libanykeep/`: shared C++ models, storage, controllers, and desktop host API.
- `libanykeep/qml/`: shared desktop/mobile QML; read its scoped `AGENTS.md`.
- `libanykeep/qml/editor/`: structured editor internals; read its scoped `AGENTS.md`.
- `src/` and `src/mobile/`: desktop and Android application shells.
- `plugins/`: optional integrations; keep plugin-specific code out of `libanykeep`.
- `tests/`: QtTest executables; read `tests/AGENTS.md` before changing tests.

Use the existing `build/qt6_Debug` tree. Prefer focused targets and CTest labels:

```sh
cmake --build build/qt6_Debug --target editor_tests -j4
ctest --test-dir build/qt6_Debug -L editor --output-on-failure
```

Use target `anykeep_all_tests` followed by unfiltered CTest for a full run.

CTest supplies the offscreen/software-rendering environment. Run executables
directly only when test arguments or verbose diagnostics are needed, and then
set `QT_QPA_PLATFORM=offscreen` and `QSG_RHI_BACKEND=software`.

Keep `NoteBlockModel` canonical, preserve the tiny root QML compatibility
facades, and do not mix desktop-only dependencies into the shared mobile core.
Preserve unrelated working-tree changes and verify with `git diff --check`.
