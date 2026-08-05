# Test map

- `noteblockmodel_test`: Markdown codec and structural model operations.
- `noteeditor_test`: editor operations, history, clipboard, and controller API.
- `editorqml_test`: structured editor QML behavior and reorder interactions.
- `desktopnoteeditorhost_test`: the thin desktop host/model bridge.
- `notesmanagerqml_test`: notes manager, folders, collections, and drag/drop.
- `settingsqml_test`: settings QML, icons, spelling, and palette behavior.
- Other `*_test` targets cover the correspondingly named C++ subsystem.

`noteblockmodel_test` remains one executable; implementations are split into
`markdown`, `operations`, `lists_tables`, `structure`, and `search_code`
sources. Manager QML uses `notesmanagerqml_shell`, `notesmanagerqml_drag`,
`notecollectionqml`, and `foldersqml`; drag setup belongs in
`ManagerInteractionFixture`, not in individual scenarios.

Put reusable Quick/QML setup and item lookup in `quicktestsupport.*`; keep test
intent and assertions in the owning test file. Prefer semantic `objectName`
lookups over walking visual-child indexes.

Focused editor verification:

```sh
cmake --build build/Desktop-Debug --target editor_tests -j4
ctest --test-dir build/Desktop-Debug -L editor --output-on-failure
```

For all desktop QML shells, use target `desktop_qml_tests` and CTest label
`qml`; use labels `notesmanager` or `settings` for either area alone.

Every test executable links `headlesstestinit.cpp`, so direct IDE runs default
to offscreen/software rendering too. Explicit environment values override the
defaults. New tests must also be registered before the final shared
`set_tests_properties` call. Warnings stay visible; routine `anykeep.*` info
logging is disabled by the CTest environment.

## Domain routing

| Changed area | Build target(s) | CTest label |
| --- | --- | --- |
| Structured editor/model | `editor_tests` | `editor` |
| Desktop QML shells | `desktop_qml_tests` | `qml` |
| Notes manager | `notesmanagerqml_test` | `notesmanager` |
| Folder catalog, rules and workspace folders | matching `folder*`, `noterule*`, `rules*`, `notesworkspacefolders_test` | `folders` |
| Draft lifecycle | `filedraftstore_test`, `draftmanagertransfer_test` | `lifecycle` |
| Shared cache/media/storage | matching storage target | `storage` |
| Nextcloud plugin | `nextcloudcategory_test`, `nextcloudworker_test`, `nextcloudstorage_test` | `nextcloud` |
| XMPP protocol/OMEMO | available `xmpp*` targets plus `privatenotespubsubitem_test` | `xmpp` |

Use `add_anykeep_test(target sources...)` for an ordinary test that links
`anykeep` and `Qt::Test`. Keep QML, Network, Xml, plugin include paths and
provider-specific link dependencies explicit next to the target.
