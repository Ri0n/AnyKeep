# Note lifecycle architecture

Status: the crash-safe draft, in-process multi-editor session, publication,
retry, and conflict foundations are implemented. Tag-based routing, cross-process
editing coordination, and fully durable publication reconciliation remain future
work. Statements under current implementation and planned improvements describe
known gaps rather than intended behavior.

This document defines the lifecycle of a logical note from opening through local
editing, publication, conflict handling, recovery, and deletion. Media byte
ownership is described separately in
[Media storage architecture](media-storage-architecture.md).

## Vocabulary and identity

- **Logical note** is the user-visible note, independent of a particular editor
  window or storage representation.
- **Editor session** is one `NoteEditor` instance, hosted by a desktop or Android shell,
  participating in the editing of a logical note.
- **Draft session** is the set of editor sessions sharing one draft UUID.
- **Checkpoint** is a durable `Editing` snapshot in `DraftStore`.
- **Origin** is the storage and note ID from which editing started. Either can be
  absent for a new or not-yet-routed note.
- **Base concurrency token** is opaque storage data captured when editing starts,
  such as an ETag, remote revision, or base version.
- **Publication destination** is selected by routing. It is not the identity of
  the draft.

The draft UUID is the authoritative identity of an editing session. It remains
stable when a new note has no remote ID, when routing changes its destination, and
when publication assigns a remote ID. `originStorageId + originNoteId` is useful
for finding an existing draft for an already published note, but is not a valid
primary identity for new or rerouted notes.

The current persistent field names are `storageId` and `remoteNoteId`. Their
initial meaning is origin. For a new or rerouted note the current implementation
also stores the resolved destination in `storageId`; separating the resolved route
from origin metadata is a future schema cleanup.

## Architectural invariants

1. While at least one editor session is open, the durable working copy is an
   `Editing` draft. The origin storage must not be published merely because an
   editor loses focus or the autosave timer fires.
2. An unchanged note does not need an initial checkpoint. Once a checkpoint
   exists, returning to the loaded baseline must itself be checkpointed so that
   stale intermediate content does not remain in DraftStore.
3. Losing focus and periodic autosave may create checkpoints. Receiving focus may
   only load a newer checkpoint from the same draft UUID; it must not reload the
   origin storage over a newer draft.
4. A draft becomes publishable only when the last editor session is explicitly
   closed, the note manager switches away from it, or the application performs an
   orderly close of that editor.
5. Failure to make the final checkpoint or state transition must keep the editor
   open whenever the UI still has control over closing.
6. The base concurrency token is captured once, at the first checkpoint, and is
   not replaced by tokens read immediately before publication.
7. A successful publication or a proven no-op is the only normal path that
   removes a publish draft.
8. Draft and cache manifests may share immutable media blobs, but publication
   state never owns or duplicates the media bytes themselves.

## Components and ownership

```mermaid
flowchart LR
    subgraph UI["UI process"]
        S1["Desktop or Android shell A"]
        S2["Desktop or manager shell B"]
        E1["NoteEditor A"]
        E2["NoteEditor B"]
    end

    subgraph Core["libqtnote"]
        DM["DraftManager<br/>sessions, checkpoints, publication"]
        DS["Encrypted DraftStore"]
        ROUTE["Routing policy<br/>planned: tags / explicit choice"]
        MGR["NoteManager"]
        CR["ConflictResolver"]
        MEDIA["LocalMediaStore"]
    end

    subgraph Backends["Storage plugins"]
        LOCAL["Local storage"]
        REMOTE["Remote storage"]
        CACHE["RemoteCacheStore"]
    end

    S1 --> E1
    S2 --> E2
    E1 -->|"acquire/release UUID lease"| DM
    E2 -->|"acquire/release UUID lease"| DM
    E1 -->|"media manifest"| MEDIA
    E2 -->|"media manifest"| MEDIA
    DM -->|"durable records"| DS
    DM -.->|"future route query"| ROUTE
    DM -->|"async load/save/remove"| MGR
    DM -->|"conflict context"| CR
    MGR --> LOCAL
    MGR --> REMOTE
    REMOTE <--> CACHE
    DS -->|"manifest references"| MEDIA
```

`NoteEditor` owns canonical in-memory content, the local dirty flag, the last
draft revision it has applied, the media manifest, and the document-wide history.
The QML editor view owns transient cursor, selection, and scroll state and exposes
it to `NoteEditor` as logical addresses. `DraftManager` owns the process-wide
draft lease count and all persistent publication transitions. `NoteStorage`
plugins own remote protocols, local file formats, and interpretation of their
opaque `backendData` concurrency tokens.

The transient editor history and its relationship to checkpoints and external
draft reloads are specified in
[Note editor undo and redo architecture](note-editor-undo-redo.md).

## Persistent draft model

The relevant `DraftRecord` fields are:

| Field | Meaning |
|---|---|
| `id` | Stable draft UUID and editing-session identity. |
| `operation` | Publish/update a note, or delete an origin note. |
| `state` | Persistent publication state described below. |
| `storageId`, `remoteNoteId` | Origin identity; currently also used for a resolved destination after routing. |
| `title`, `body`, `format`, `tags` | Canonical note contents used for publication and recovery. |
| `backendData` | Base concurrency token captured before editing. |
| `media` | Manifest references into the shared local media store. |
| `revision` | Monotonic checkpoint sequence within one draft UUID. |
| `updatedAt`, `retryAt`, `lastError` | Recovery, retry scheduling, and diagnostics. |

`revision` orders checkpoints of the same draft. It is not a remote revision and
must never be passed to a storage backend as a concurrency token.

## Persistent state machine

```mermaid
stateDiagram-v2
    [*] --> Absent
    Absent --> Editing: first changed checkpoint
    Editing --> Editing: checkpoint / revision++
    Editing --> Ready: last editor closes and destination known
    Editing --> NeedsRouting: last editor closes without destination
    NeedsRouting --> Ready: route resolved
    Ready --> Publishing: publication worker starts
    Retry --> Publishing: retry time reached or manual retry
    Publishing --> Publishing: restart recovers interrupted attempt
    Publishing --> [*]: save succeeds
    Publishing --> [*]: current source equals draft / no-op
    Publishing --> Retry: retryable storage error
    Publishing --> Retry: non-retryable error / paused
    Publishing --> Editing: conflict kept for recovery
    Publishing --> Ready: conflict creates copy and destination known
    Publishing --> NeedsRouting: conflict creates copy without destination
    Editing --> [*]: explicit discard or note deletion
```

`Retry` with a valid `retryAt` is automatic. `Retry` without `retryAt` is paused
and requires a configuration change or user action. `Publishing` is persistent so
that a crash does not lose the draft; startup treats it as an interrupted attempt
and processes it again.

A delete operation starts at `Ready`, moves through `Publishing`, and either
finishes or enters `Retry`. It does not use the editing states because the publish
draft has already been discarded before removal is queued.

## Opening and checkpointing

Opening an editor follows this precedence:

1. Use the explicitly supplied draft UUID, if the manager or recovery UI opened a
   draft.
2. Reuse the process-local UUID already associated with the origin note.
3. Look for the latest persisted `Editing` draft with the same origin as a
   compatibility/recovery fallback.
4. Otherwise create a new UUID. No persistent draft is written until content
   actually changes.

If an `Editing` record exists, its content, media manifest, base concurrency
token, and revision are applied before the editor is initially rendered. If not,
the note is loaded from its local storage or remote cache/backend.

The editor's `dirty` state means "the current in-memory editor has changes not
yet checkpointed". Clearing it after a successful checkpoint does not mean the
note is published or equal to the origin.

```mermaid
sequenceDiagram
    actor U as User
    participant E as NoteEditor
    participant DM as DraftManager
    participant DS as DraftStore

    U->>E: Edit note
    E->>E: dirty = true

    alt focus loss or autosave timeout
        E->>DM: saveEditing(draft UUID, contents)
        DM->>DS: read existing record
        Note over DM: Preserve origin and base token<br/>from first checkpoint
        DM->>DS: atomic write Editing, revision + 1
        alt checkpoint succeeds
            DS-->>E: success
            E->>E: dirty = false
        else checkpoint fails
            DS-->>E: durable error
            E->>E: remain dirty
        end
    end
```

The autosave timer reduces, but cannot eliminate, loss of edits made after the
last successful checkpoint. On Android, entering the background or an application
inactive state should request a checkpoint without marking the draft `Ready`;
process termination then loses at most the uncheckpointed interval.

A shell does not expose a manual Save command. Draft checkpointing is automatic
and is also requested before Share, Export, navigation away, or orderly close.
Share and Export copy content to an external destination; they do not replace the
draft lifecycle or make a draft publishable.

## Multi-editor sessions

Multiple `NoteEditor` instances in one process can share one draft UUID. The
process-wide lease count prevents the first closed window from publishing content
while another editor is still open.

```mermaid
sequenceDiagram
    participant A as Editor A
    participant B as Editor B
    participant DM as DraftManager
    participant DS as DraftStore

    A->>DM: acquire(origin)
    DM-->>A: draft UUID D, lease count 1
    B->>DM: acquire(origin)
    DM-->>B: draft UUID D, lease count 2

    A->>DM: checkpoint D
    DM->>DS: write Editing revision 4

    B->>DM: focus received, local revision 3
    DM->>DS: load D
    alt B has no uncheckpointed edits
        DS-->>B: revision 4
        B->>B: apply contents and revision
    else B is locally changed
        Note over B: Keep local text; never overwrite dirty editor
    end

    A->>DM: close / release D
    Note over DM: lease count 1; keep Editing
    B->>DM: final checkpoint and close
    DM->>DS: transition D to Ready or NeedsRouting
    B->>DM: release D
```

Current coordination is intentionally conservative:

- clean editors refresh from DraftStore on focus when `revision` increases;
- dirty editors are not overwritten automatically;
- concurrent dirty editors use last successful checkpoint wins semantics;
- cursor, selection, undo stack, and operation-level merges are not shared;
- leases are process-local, not cross-process.

True simultaneous collaborative editing would require operation IDs and a merge
model such as OT or CRDT. It should not be approximated by silently merging whole
Markdown snapshots.

## Closing an editor

Closing a standalone window, switching the manager preview, closing the manager,
and an orderly application exit use the same protocol:

```mermaid
flowchart TD
    CLOSE["Close or switch requested"] --> DIRTY{"Editor changed?"}
    DIRTY -->|Yes| SAVE["Checkpoint Editing draft"]
    SAVE --> OK{"Checkpoint succeeded?"}
    OK -->|No| BLOCK["Keep editor open and report failure"]
    OK -->|Yes| LAST{"Last lease for UUID?"}
    DIRTY -->|No| LAST
    LAST -->|No| RELEASE["Release lease; remain Editing"]
    LAST -->|Yes| EXISTS{"Editing draft exists?"}
    EXISTS -->|No| RELEASE
    EXISTS -->|Yes, destination known| READY["Persist Ready"]
    EXISTS -->|Yes, no destination| ROUTE["Persist NeedsRouting"]
    READY --> RELEASE
    ROUTE --> RELEASE
```

An unchanged note with no draft only releases its lease. Closing a clean editor
must still inspect DraftStore because another editor may have created the shared
checkpoint after this editor was opened.

## Routing and destination selection

The intended destination precedence is:

```text
explicit user choice or tag-based routing
    -> origin storage, if still valid
    -> highest-priority accessible storage from settings
```

An explicit choice should be expressed as routing policy, not as draft identity.
Changing the route must not change the draft UUID. Publishing to a storage other
than the origin creates a new remote note and must not silently delete or overwrite
the original.

Current implementation: there is no general router service yet. A draft with a
non-empty `storageId` publishes to that origin. `NeedsRouting` immediately chooses
`NoteManager::defaultStorage()` and clears the remote note ID. This implements the
origin/default fallback but not tag rules or a user-selected override.

## Publication and optimistic concurrency

For an existing origin note, publication first loads the current storage version.
This may be served by a durable remote cache or require a network read.

```mermaid
sequenceDiagram
    participant DM as DraftManager
    participant DS as DraftStore
    participant S as NoteStorage
    participant CR as ConflictResolver

    DM->>DS: persist Publishing
    DM->>S: load current origin note

    alt load fails
        S-->>DM: typed StorageError
        DM->>DS: Retry with error and optional retryAt
    else current contents equal draft
        S-->>DM: current note
        DM->>DS: remove draft as proven no-op
    else contents differ
        S-->>DM: current note and current token
        Note over DM: Replace current token with base token<br/>captured before editing
        DM->>S: save draft contents with base token

        alt save succeeds
            S-->>DM: saved note, remote ID and new token
            DM->>DS: remove draft
        else optimistic concurrency conflict
            S-->>DM: Conflict
            DM->>DS: preserve recoverable Editing draft
            DM->>CR: local draft + remote note + error
        else other failure
            S-->>DM: typed StorageError
            DM->>DS: Retry or paused Retry
        end
    end
```

Loading the current remote note and then saving with its newly loaded token would
silently rebase over concurrent changes. QtNote therefore restores the original
base token from `DraftRecord::backendData` immediately before saving. Storage
plugins are responsible for using that token in a conditional write.

The pre-publication no-op comparison includes title, body, format, and media
manifest. Concurrency metadata is deliberately excluded: if another device
produced identical user-visible content with a newer token, keeping that version
is safe. This avoids storing a full original-content snapshot in every draft, but
means a no-op cannot be proven while both the origin and its cache are unavailable.
The draft remains durable and retries in that case.

## Conflict handling

Conflict detection belongs to the storage backend; conflict policy belongs to a
`ConflictResolver`. Before invoking a possibly asynchronous resolver,
`DraftManager` returns the local record to `Editing` so a crash or UI lifetime
change cannot lose it.

```mermaid
flowchart TD
    C["Storage reports Conflict"] --> SAFE["Persist recoverable Editing draft"]
    SAFE --> POLICY{"Resolver decision"}
    POLICY -->|CreateCopy| COPY["Clear remote ID and base token"]
    COPY --> DEST{"Destination available?"}
    DEST -->|Yes| READY["Ready"]
    DEST -->|No| ROUTING["NeedsRouting"]
    POLICY -->|KeepDraft| KEEP["Editing with lastError"]
    POLICY -->|Discard| DROP["Remove local draft; keep remote"]
```

The default lossless policy creates a new local copy rather than overwriting the
remote version. A future interactive resolver may expose all three decisions, but
must invoke its completion callback exactly once.

## Error handling contract

| Failure | Persistent result | UI/application behavior |
|---|---|---|
| Draft key, crypto, or store initialization fails | No safe editor state is available | Abort startup; do not permit editing without crash-safe storage. |
| Checkpoint write fails | Existing draft remains; editor stays dirty | Block explicit close where possible and surface a durable error. |
| Draft reload on focus fails | Current editor contents remain untouched | Report diagnostics; never fall back to overwriting from origin storage. |
| No routing destination is available | `NeedsRouting` | Keep the draft and retry when storage availability changes. |
| Storage unavailable or retryable network/I/O error | `Retry` with `retryAt` | Exponential backoff, currently bounded to 30–300 seconds. |
| Authentication/configuration/non-retryable error | `Retry` without `retryAt` | Pause automatic retries and notify the user to fix configuration or choose a route. |
| Existing-note optimistic concurrency failure | Recoverable `Editing` draft | Run conflict policy; never overwrite with a freshly loaded token. |
| Storage plugin disabled during publish | Publish draft becomes `NeedsRouting`; delete remains paused `Retry` | Cancel owned jobs, preserve local draft, leave remote original unchanged. |
| Process crashes in `Ready`, `Publishing`, or `Retry` | State remains in DraftStore | Resume processing on startup. |

Error paths must themselves be checked. In particular, failure to persist `Retry`
or failure to remove a draft after confirmed publication is not equivalent to a
normal backend failure: it leaves publication outcome ambiguous and requires
reconciliation rather than an unconditional repeat.

Current limitation: several secondary DraftStore failures in publication and
retry cleanup are only logged or ignored. They do not normally lose the draft,
but they can leave an ambiguous `Publishing` record and must be made first-class
outcomes before the protocol is considered transactionally complete.

## Crash windows and recovery

```mermaid
flowchart TD
    START["Application startup"] --> OPEN{"DraftStore opens and authenticates?"}
    OPEN -->|No| FATAL["Abort editing startup"]
    OPEN -->|Yes| SCAN["Scan persistent records"]
    SCAN --> EDIT["Editing: offer/open recovery editor"]
    SCAN --> READY["Ready: publish when destination is accessible"]
    SCAN --> PUB["Publishing: treat as interrupted and reconcile/retry"]
    SCAN --> RETRY["Retry: wait for retryAt or user/config change"]
    SCAN --> ROUTE["NeedsRouting: resolve route when storage is ready"]
```

The important crash windows are:

- before the first successful checkpoint: edits since the last checkpoint can be
  lost; focus-loss, timer, and mobile-background checkpoints limit this window;
- after checkpoint and before close: `Editing` is recoverable and never published
  automatically;
- after `Ready` and before remote save: startup safely resumes publication;
- after an existing-note conditional save is accepted but before draft removal:
  the next attempt can usually be reconciled by remote ID and content;
- after a new-note save is accepted but before its assigned remote ID is persisted:
  retry can create a duplicate unless the backend supports idempotency or lookup
  by a stable client operation ID;
- after a media blob is committed but before its manifest: an unreferenced blob is
  safe and later garbage-collected;
- a manifest must never commit before every referenced media blob is durable.

The new-note acknowledgement window is the most important remaining durability
gap. The publication protocol should eventually persist an idempotency key or a
backend-specific reconciliation receipt before removing the draft.

## Deletion

Deleting a note has two local actions:

1. discard an unpublished editing draft for that logical note;
2. queue a durable `Delete` record for a non-empty origin storage and note ID.

The queued delete is asynchronous and retryable. Disabling its storage must not
reroute deletion to another backend; the record remains paused until the original
storage is available or the user explicitly abandons it.

## Current implementation boundaries and planned improvements

The current structure is sound enough to evolve, but the following boundaries
should be made explicit before adding more backends and Android lifecycle hooks:

1. Extract a `NoteRouter` policy from `DraftManager`, with inputs for tags, origin,
   explicit user override, accessible storages, and configured priority.
2. Separate immutable origin fields from the resolved publication route in the
   persistent schema, or persist only origin plus an explicit route decision.
3. Make DraftStore transitions compare-and-swap operations using draft UUID,
   revision, and expected state. This prevents stale editors or workers from
   overwriting newer state.
4. Split publication orchestration, retry scheduling, and editor lease tracking
   out of `DraftManager` as these responsibilities grow.
5. Add stable publication operation IDs and backend reconciliation for the crash
   window after remote acknowledgement.
6. Replace opaque, convention-only concurrency keys with a documented storage
   token contract while retaining an escape hatch for backend-specific data.
7. Surface checkpoint and paused-publication errors through persistent UI, not
   only logs and transient notifications.
8. Add an event-driven draft revision signal if editors need live synchronization;
   focus-based refresh remains a useful fallback.
9. Add cross-process locking or a single-instance editing broker before allowing
   separate QtNote processes to edit the same profile.
10. Check and persist the outcome of every DraftStore mutation, especially after
    a remote side effect, and route ambiguous results through reconciliation.

## Required tests

The lifecycle should be covered with an injectable DraftStore, NoteStorage, clock,
router, and conflict resolver. At minimum, tests must cover:

- first checkpoint captures origin and base token exactly once;
- two editors share a UUID and only the final close marks it publishable;
- a clean editor adopts a newer revision while a dirty editor is not overwritten;
- returning to the origin content replaces an older checkpoint and publication is
  skipped as a no-op;
- each DraftStore write/transition/remove failure preserves recoverability;
- retryable and paused errors produce the correct `Retry` state;
- crash recovery from every persistent state;
- optimistic concurrency conflict and all resolver decisions;
- storage removal during load, save, delete, and retry;
- routing precedence: explicit/tag route, origin, then configured default;
- new-note publication reconciliation after a simulated lost acknowledgement;
- media manifest durability and garbage-collection roots across draft/cache moves.
