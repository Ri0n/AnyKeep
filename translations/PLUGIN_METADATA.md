# Plugin metadata translations

Plugin titles and short descriptions use a separate Qt Linguist TS resource so they can be translated in Transifex
without loading plugin libraries or compiling `.qm` files.

## Source files

- `plugin_metadata_en.ts` is the English source catalog.
- `plugin_metadata_<locale>.ts` contains Transifex translations.
- Each plugin has a static `plugin-metadata.json` descriptor that refers to stable TS message IDs through `nameId`
  and `descriptionId`.

Message IDs use this form:

- `plugin.<plugin-id>.name`
- `plugin.<plugin-id>.description`

The second QT resource in `transifex.yml` maps the English catalog to all locale catalogs. Do not run `lupdate` over
these files: they are maintained as an explicit Transifex resource, independently from strings discovered in C++,
QML, and UI files.

## Build-time conversion

During CMake configuration, `cmake/generate_plugin_metadata.py` combines:

1. the static plugin descriptor;
2. the English source strings and every finished translation from the TS catalogs;
3. the plugin icon, embedded as Base64;
4. the current AnyKeep version where a descriptor uses `"current"`.

`version`, `minVersion`, and `maxVersion` are ordinary JSON strings and must follow SemVer 2.0.0, for example
`"1.2.0"`, `"3.3.0-rc.1"`, or `"3.3.0+distribution.2"`. Build metadata does not affect compatibility
comparison. Hexadecimal packed versions are not supported.

The resulting self-contained JSON file is passed to `Q_PLUGIN_METADATA`. `PluginManager` can therefore read a
plugin's identity, localized text, compatibility range, capabilities, load policy, and icon through
`QPluginLoader::metaData()` without constructing the plugin object.

All dynamic plugins use the common `ANYKEEP_PLUGIN_INTERFACE_IID`; their stable, unique identity is the `id` field
inside the generated JSON.

Desktop-specific plugins declare a top-level `desktopEnvironments` string array, for example
`["cinnamon", "x-cinnamon"]`. In `Automatic` mode, `PluginManager` considers such a plugin only when one of these
values matches the current desktop session. An explicit `Enabled` policy overrides this filter.

No `lrelease` step is required for metadata strings. Pulling updated TS files from Transifex and reconfiguring the
build is sufficient. CMake watches the descriptor, icon, generator, and every metadata TS catalog and regenerates the
embedded JSON when any of them changes.

A translation is embedded only when its TS `source` still matches the English source and it is not marked
`unfinished`, `vanished`, or `obsolete`. Otherwise runtime locale fallback uses the language-only translation and
then English.
