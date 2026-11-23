# Portable backup format (issue #205)

## Status

This documents the **readable archive foundation only**, not yet an Android
backup/restore feature. The archive writer does not enumerate PTF notes,
drafts, attachments, settings or provider credentials. No UI should describe
a backup as complete until all requested content is captured and verified.

## Readable format, version 1

An uncompressed USTAR tar archive, readable by standard archive managers
without AnyKeep or the old Android Keystore. Input is streamed once, rather
than materializing the whole backup in memory.

The last ordinary file is manifest.json, followed by two empty USTAR blocks.
Its structure is:

```json
{
  "format": "anykeep-portable-backup",
  "version": 1,
  "mode": "readable",
  "createdUtc": "2026-11-24T00:00:00.000Z",
  "entries": [
    {
      "path": "notes/example.md",
      "kind": "note",
      "mimeType": "text/markdown",
      "bytes": 42,
      "sha256": "<64 lowercase hexadecimal digits>",
      "id": "stable-note-id",
      "providerId": "local-provider"
    },
    {
      "path": "media/example/photo.png",
      "kind": "attachment",
      "mimeType": "image/png",
      "bytes": 1024,
      "sha256": "<64 lowercase hexadecimal digits>",
      "id": "attachment-id",
      "noteId": "stable-note-id"
    }
  ]
}
```

The example hashes are placeholders, not valid backup hashes.

Paths must be safe, relative, ASCII-only USTAR entry names of at most
99 bytes; user-facing Unicode filenames belong in metadata or note bodies.
Entries are ordinary files, never symlinks, hardlinks, directories, device
files or PAX/GNU extensions. Local Android content URIs and filesystem paths
must never become portable locators.

A reader checks all headers, lengths, entry types, path components,
duplicates, padding, SHA-256 checksums, schema and complete
manifest-to-file mapping **before** modifying application state.

Limits: 50,000 payload entries; 8 GiB minus one byte per entry; 128 GiB
cumulative payload; 4 MiB manifest. They bound untrusted inputs.

## Security boundary

SHA-256 in a **readable** tar detects corruption or missing assets but
does not authenticate the archive creator: an attacker can replace both data
and hashes. Password-protected backup must authenticate the entire archive
using a separately derived key and a hardened password KDF such as Argon2id.

Readable backups must not contain local encryption keys, passwords,
provider tokens or OMEMO state. The snapshot producer must enforce that
boundary; the archive writer merely serializes explicitly supplied sources.

## Remaining work

- Capture consistent checkpoints from live NoteEditor and DraftManager,
  including pending edits and unsynced publication.
- Enumerate enabled providers; explicitly report coverage and exclusions.
  Never infer that remote synchronization backs up local-only state.
- Decrypt and copy LocalMediaStore data into portable attachment entries,
  accounting for external sources and rewriting every reference.
- Stage and verify the archive locally, then stream it to Android SAF and
  report cancellation/short writes accurately.
- Add authenticated encrypted backups, with explicit informed consent for
  eligible sensitive state and wrong-password/tamper tests.
- Implement preflight + rollback-safe restore under a new local key, with
  merge/replace semantics, collision handling and explicit external-file
  rebinding.
- Integrate Settings → Backup and restore and a guided Android uninstall
  confirmation only once backup and restore are usable.

The existing single-note Markdown export is a different feature and is
not a whole-device backup.
