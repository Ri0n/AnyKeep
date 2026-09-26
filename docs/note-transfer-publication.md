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

## Cancellation versus remote side effects

Cancelling or retargeting a publication only changes local intent. It does not
establish that the remote service did nothing.

For a side-effecting create-save, DraftManager keeps enough attempt context to
observe a late terminal result. A late ACK is reconciled rather than ignored:
an abandoned destination is deleted durably, while an ACK for the still-current
target supplies the remote identity for that same logical draft.

## Routing evaluation boundary

Routing is evaluated when an editing lifecycle is committed, not at periodic
autosave/focus-loss checkpoints.

An `Editing` draft is durable but semantically provisional. In particular,
routing may depend on the user's final:

- tags;
- title/body or other structured content;
- folder and metadata;
- attachments or capabilities;
- additional routing inputs introduced in the future.

Routing must not be modeled as merely "choose a storage". Retargeting to another
storage is one possible routing action, but the routing pipeline may perform
multiple actions and will grow independently of storage selection.

Therefore an autosave checkpoint must **not** enter the routing/publication
pipeline. Doing so could evaluate incomplete tags or other provisional state,
publish to the wrong destination, or trigger actions which would then need to be
undone when the user keeps editing.

The normal commit boundary is final logical close:

```text
Editing + open views
    -> autosave/checkpoint (still Editing)
    -> final view closes
    -> Ready / NeedsRouting
    -> evaluate routing from final canonical state
    -> publication transaction
```

Explicit lifecycle commands (Move, Copy, Trash, Delete) are separate user
decisions with their own transaction rules. They do not weaken the rule that a
periodic editing checkpoint is non-publishable.

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

Recycle is a local two-phase commit:

1. `prepareForRecycle()` checkpoints the canonical model, closes views and
   writes the recycle folder intent into the same draft, but deliberately keeps
   that draft `Editing`;
2. the caller persists FolderCatalog/native folder metadata;
3. only after that succeeds does `retryDraftNow()` move the draft to
   `Ready`/`NeedsRouting` and allow publication.

If catalog/native-folder persistence fails, the draft cannot race ahead and
publish a recycle transition that the local catalog never committed.

## Format conversion boundary

Destination compatibility is validated while selecting/retargeting a storage,
but the durable/live document stays in its canonical format.

At publication time DraftManager derives a target-supported title/body/format
snapshot and submits that to `NoteStorage`. Existing-note no-op comparison also
uses this derived representation.

## Copy

Copy creates a new draft UUID, clears source-removal fields and remote identity,
and publishes independently. Copy is a second logical note; move is not.
