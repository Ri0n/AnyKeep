# AnyKeep Nextcloud Notes storage

This directory is intended to be copied to `AnyKeep/plugins/nextcloud`.

## Integration

1. Add `nextcloud` to `plugins_list` in `plugins/CMakeLists.txt`.
2. Reconfigure CMake and build AnyKeep.
3. Enable the plugin and enter:
   - the base Nextcloud URL, including any installation subdirectory;
   - the Nextcloud user name;
   - an app password;
   - a request timeout.

Example server URL:

```text
https://cloud.example.com
```

For a subdirectory installation:

```text
https://example.com/nextcloud
```

The code appends `/index.php/apps/notes/api/v1`.

## Behavior

- `noteList()` downloads metadata only; note content is loaded lazily.
- Network requests run in a dedicated worker thread.
- The asynchronous storage API executes network requests in the worker thread and reports completion through jobs.
- Updates send `If-Match` with the last known note ETag.
- HTTP 412 is treated as a conflict. The local text is preserved and the server version is not overwritten.
- Server-sanitized titles are adopted from the API response.
- `category`, `readonly`, and `etag` are preserved as opaque backend attributes of the generic note data.
- `favorite` is exposed through the generic note API and can be changed directly from the note editor.
- There is no persistent offline cache and no three-way merge in this first version.

## Credential storage

This minimal integration stores the app password in `QSettings`, matching the plugin's other settings.
For a distributable production build, replace that field with a platform keychain integration such as QtKeychain.

## Validation performed

The source layout and overrides were checked against the current public AnyKeep
`master` interfaces. This archive was not fully compiled in the generation
environment because Qt development packages and the complete AnyKeep build tree
were not available there.
