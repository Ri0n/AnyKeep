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

## Snapshot collection

BackupSnapshotJob enumerates registered providers, including inaccessible
ones, through their asynchronous list and load jobs. It records failed
full loads (while preserving a readable cached body if supplied by the
list), marks offline providers and providers changed during collection
incomplete, and applies the application-wide folder assignments. It does
not use the synchronous network APIs. DraftManager::backupRecords captures
durable intents and dirty canonical editors at the start, without a
checkpoint write or changing publication eligibility.

Published notes and local draft versions are separate entries. The draft
group also records delete and transfer intents descriptively in the
manifest, including original storage/note IDs, draft IDs and state. A
restore must not replay those operations automatically. Opaque provider
backend state is excluded. This is a per-note snapshot collected over
time, not a distributed transaction across independent remote providers.

## Export operation

BackupExportJob joins snapshot collection, asynchronous keychain preparation
on the application thread, and PTF writing on a dedicated worker. Its phases
are Collecting, PreparingMedia and Writing, with phase-local progress. It
never blocks the application event loop waiting for a remote storage or the
keychain. The worker uses only detached snapshots and the initialized media
store. Cancellation stops collection or cooperatively stops copying, including
between media chunks and staged file readback chunks. A final publish gate
arbitrates cancellation against the directory rename; once the rename is
authorized, report the actual write outcome. Destroying the job requests
cancellation and joins the writer.

The manifest includes a sorted inventory of all staged regular files other
than the manifest itself, including hidden PTF indexes. Each entry has a
relative path, decimal byte size and SHA-256 read from disk. This supports
later input/destination verification; it is not yet an authenticated manifest,
and does not establish that a separate SAF copy has been verified.

## Read-only inspection

PortableBackup::inspectDirectory reads the bounded versioned manifest and
returns typed provider, note, draft, missing-media and file metadata for the
restore preview. It does not construct PTFStorage, resolve a media key, import
attachments, or mutate any provider. It recomputes coverage and counts, checks
the required PTF note and folder-index paths, rejects duplicate or aliased
paths, unexpected files and directories, traversal, symbolic links and
redirected entries, and streams every inventoried file to verify its size and
SHA-256. Inspection can be cancelled between read chunks. A consistent but
incomplete export is a valid preview with complete=false.

Only the current inventory-bearing directory format is accepted; a manifest
without an inventory cannot be verified and is rejected. Both writer and
reader enforce the same 32 MiB manifest limit. Portable note copies and
sidecar attachment names avoid case-folded aliases, and reserve PTF metadata
filenames instead of overwriting attachments with sidecar indexes.

The plaintext manifest is not authenticated. Its digest identifies the
inspected manifest for subsequent private staging, not the identity of the
author. Before actual restore, copy into private staging and revalidate there;
a successful inspection of an externally writable directory does not make it
immutable. The inspection does not validate all PTF document semantics or
authorize any automatic remote operation.

## Remaining work before exposing a user-facing backup

1. Capture provider-specific safe recovery metadata, recycle-bin history,
   partial media availability and safe application settings. Stage and
   revalidate input privately before reading its PTF contents. Prevent
   destructive follow-up actions if coverage or verification fails.
2. Implement optional password protection using a password KDF and a separate
   SecureEnvelope domain, with authenticated file identities and an authenticated
   manifest; protect filenames as well as file contents when confidentiality
   is selected. Explicitly distinguish eligible recovery secrets from
   device-local keys and OMEMO state. Do not leave an unencrypted export at
   the user-selected destination in protected mode.
3. Use an existing archive implementation if single-file transport is selected.
   Android uses SAF for the file or directory destination; desktop can use the
   same core with ordinary paths. Verify external destination writes before
   offering the system uninstall confirmation.
4. Restore by validating and staging the entire input, opening ordinary PTF
   with the new profile media key, and reusing existing copy/draft/conflict
   paths. Import must preserve existing data and handle unavailable providers
   explicitly. No automatic remote deletion or overwrite.

No Settings action or complete restore is exposed yet.
