# Portable backup and restore (issue #205)

## Design

PTF is the portable note representation. A backup is a directory containing
one ordinary PTF storage per source provider (or draft group), together with
a small origin manifest. Markdown notes and sidecar attachments are usable
without AnyKeep. Compression/archiving and password protection are independent
optional transport transformations, not a new note storage format.

The shared core owns snapshot conversion and persistence. Android SAF and
desktop file selection are destination adapters. Neither is allowed to register
a temporary storage in NoteManager, modify the profile's PTF location, or
trigger publication of a backup copy.

## Implemented first stage

- Fixed-path PTF instances are independent of global storage configuration and
  legacy migration, including on Android.
- PTF materializes attachments through the existing authenticated MediaSource
  interface. It streams chunked media, handles externally linked files, and
  preserves relative references and sidecar metadata.
- PortableBackup::exportDirectory accepts loaded snapshots and the folder tree,
  creates a separate PTF directory for each source storage, and records the
  source storage ID, original note ID, optional draft ID, original format,
  tags, modification timestamp, and unavailable attachment references.
- The result explicitly distinguishes an incomplete export from a complete
  export. A missing attachment keeps its original URI and is recorded in the
  manifest. Failed destination writes and cancellation do not publish a
  destination directory. A staged tree is renamed into place only after the
  manifest is committed.
- No opaque provider backendData, remoteData, or keychain secrets are copied.
  Provider-specific nonsecret conflict tokens require an explicit portable
  representation before remote restore is implemented.

The existing single-note Markdown export remains separate.

## Directory layout

    backup/
      manifest.json
      storage-1/
        .anykeep-folders.json
        Example.md
        Example/
          .anykeep-media.json
          diagram.png
      storage-2/
        ...

Each storage directory can be opened as ordinary PTF. The versioned origin
manifest maps directory and PTF note ID back to the source provider and note
or draft ID. It also reports provider coverage and unavailable attachments.
Provider IDs are metadata, never filesystem path components. Missing media are
not silently removed from the source note. The current writer does not yet
capture available portions of an incomplete media download.

## Remaining work before exposing a user-facing backup

1. A controller obtains consistent checkpoints from live NoteEditor and
   DraftManager without changing publication state, enumerates all selected
   providers and loads notes through asynchronous storage jobs. It must report
   omissions and races, and preserve pending edit/deletion intent without
   replaying it automatically. The directory writer runs on a worker with the
   media key already resolved on the application thread.
2. Capture provider-specific safe recovery metadata, recycle-bin history,
   partial media availability and safe application settings. Verify the
   resulting file inventory and SHA-256 before declaring a complete backup.
3. Implement optional password protection using a password KDF and a separate
   SecureEnvelope domain, with authenticated file identities and an authenticated
   manifest; protect filenames as well as file contents when confidentiality
   is selected. Explicitly distinguish eligible recovery secrets from
   device-local keys and OMEMO state. Do not leave an unencrypted export at
   the user-selected destination in protected mode.
4. Use an existing archive implementation if single-file transport is selected.
   Android uses SAF for the file or directory destination; desktop can use the
   same core with ordinary paths. Verify external destination writes before
   offering the system uninstall confirmation.
5. Restore by validating and staging the entire input, opening ordinary PTF
   with the new profile media key, and reusing existing copy/draft/conflict
   paths. Import must preserve existing data and handle unavailable providers
   explicitly. No automatic remote deletion or overwrite.

No Settings action or complete restore is exposed by this first stage.
