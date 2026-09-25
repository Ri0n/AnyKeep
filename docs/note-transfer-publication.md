# Note transfer and publication

Read this for routing, cross-storage move/copy, format conversion and source
deletion.

## Canonical document versus storage representation

`DraftRecord` stores the canonical logical document. Changing target storage
must not destructively convert title/body/format.

Storage-specific conversion occurs only when submitting a publication snapshot:

```text
canonical draft -> choose target -> convert representation -> NoteStorage save
```

Thus `PTF -> XMPP -> plain-text backend -> PTF` before acknowledgement cannot
degrade the canonical document through repeated conversion.

## Persistence identities

Ordinary edit:

- `storageId + remoteNoteId`: persisted object;
- `backendData`: its base concurrency token.

Cross-storage move before destination ACK:

- `storageId`: current destination;
- `remoteNoteId`: empty;
- `removeSourceStorageId + removeSourceNoteId`: persisted source;
- `backendData`: still the source concurrency token.

The source token is retained so returning to the source before ACK is lossless.
It must never be passed wholesale to the new backend. Portable user metadata
(such as Favorite when supported) may be projected separately.

## Open-note move

For a shared live model:

1. checkpoint dirty content;
2. ensure one Editing draft exists;
3. retarget that same draft;
4. update the shared editor persistence identity;
5. keep every view open.

The draft UUID never changes.

If the destination equals the pending source before ACK, the move is cancelled:
restore source storage/remote ID, clear `removeSource*`, retain the original
base concurrency token.

## Closed-note move

With no live model, `stageTransfer()` persists the canonical source document,
records `removeSource*`, marks the destination Ready and lets publication run.

Workspace `pendingMoves_` is UI/reorder bookkeeping only. Source deletion is
owned exclusively by DraftManager.

## Publication ordering

Cross-storage move ordering is strict:

```text
destination save ACK
    -> persist destination identity/token
    -> durably queue source Delete
    -> remove transfer draft
```

Source deletion never starts before destination acknowledgement **as part of a
continuing move**. Explicit permanent Delete is different: it cancels the move
and owns every persisted identity of the logical note. Before ACK that means the
source; in the post-ACK/pre-cleanup window it means both destination and source.
DraftManager queues those Delete intents durably before it drops the transfer
record.

Recycle also cancels the user's active editing lifecycle but **preserves the
latest canonical contents**. `DraftManager::prepareForRecycle()` is the shared
persistence boundary used by manager and standalone UI:

- first checkpoint the current shared model when one is live;
- pre-ACK transfer: cancel the move back to the original source identity, keep
  the source concurrency token, set the same draft's folder to Recycle Bin and
  publish that source update;
- post-ACK transfer with source cleanup pending: keep the acknowledged
  destination as the target, set Recycle Bin on the same durable transfer, and
  retain `removeSource*` until successful destination publication safely queues
  source deletion;
- ordinary edited draft: keep the draft, set Recycle Bin and publish current
  content/metadata;
- clean persisted note with no draft: recycle the existing object directly;
- never-published local draft: close/discard it because no storage object exists
  to place in a recycle bin.

FolderCatalog owns the immediate recycle-bin projection/metadata; it does not
interpret transfer fields or discard publish state.

## Format conversion boundary

Destination compatibility is validated while selecting/retargeting a storage,
but the durable/live document stays in its canonical format.

At publication time DraftManager derives a target-supported title/body/format
snapshot and submits that to `NoteStorage`. Existing-note no-op comparison also
uses this derived representation.

## Copy

Copy creates a new draft UUID, clears source-removal fields and remote identity,
and publishes independently. Copy is a second logical note; move is not.
