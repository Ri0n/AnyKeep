# Folders and rules implementation plan

## Status

Active implementation plan. This document records the product decisions before
the implementation is spread through the note model, storage plugins and QML.
Each numbered implementation stage is independently buildable, tested and
committed locally. Commits must not be pushed without explicit approval.

## Product decisions

- The user-facing term is **folder**. Internal APIs also use folder, rather
  than category or group.
- There is one global QtNote folder tree, shared by all storages. A storage
  that can persist folder information contributes its tree and assignments to
  this global tree when it is connected. The catalog merges those trees.
- A note belongs to zero or one folder. Notes without an assignment are shown
  in the virtual **Unsorted** folder; Unsorted is not persisted and cannot be
  renamed, moved, archived or deleted.
- Folders can be nested, reordered and moved to any depth. Folder creation is
  at the root by default; creation from a folder's context menu makes it a
  child of that folder.
- Folder rows use folder icons, may be collapsed, and use the same animated
  structural drag model as task lists. Dropping on a collapsed folder assigns
  or reparents into it, but hovering during a drag does **not** auto-expand it.
- Folder renaming is inline. Enter and focus loss commit the staged name;
  Escape cancels it. A failed durable write leaves the edit visible with a
  retry/error state instead of silently losing it.
- Drag and drop is the normal way to move a folder to the root, so there is no
  redundant “Move to root” context-menu command.
- Favorite folders are presented first in folder pickers and menus. Archived
  folders are hidden from normal menus and pickers, except when showing the
  note currently assigned to one; they remain visible and manageable in the
  Folders tab.
- A note can be assigned from its editor through a folder picker as well as by
  drag and drop in the manager.

## Tomboy compatibility

QtNote must not write folder data into Tomboy XML, Tomboy tags, or an invented
Tomboy notebook convention. Existing QtNote tags continue to map to Tomboy
tags exactly as they do today.

Folder membership for Tomboy is therefore a local encrypted overlay keyed by
the stable pair (storageId, noteId). Rules may assign folders after Tomboy
notes are read. The same overlay is also the fallback for a storage that has
no native folder capability. This preserves Tomboy interoperability and avoids
claiming a meaning in Tomboy's specification that it does not have.

## Scope boundaries and terminology

The implementation distinguishes three related but different concepts:

| Term | Meaning | Persistence owner |
| --- | --- | --- |
| Folder | A node in the single QtNote folder tree | global FolderCatalog, plus providers that can synchronize it |
| Folder assignment | The one-folder membership of one note | note/draft/provider metadata, with a local overlay fallback |
| Tag | Existing free-form note metadata | the note and its storage's native tag support |

Folders are not a replacement for tags. A note may have many tags and at most
one folder. A folder is not a storage: moving a note into a folder never
implicitly changes the storage unless a rule or an explicit storage move says
so. The Rule action which chooses a storage is separate from its folder action.

The first delivery includes creation, inline rename, reparent/reorder,
collapse, favorite, archive, assignment, Unsorted, and recovery of the local
catalog. It deliberately does not include:

- folder sharing/permissions independent of a storage;
- a new Tomboy XML or notebook interpretation;
- automatic expansion of folders under a drag pointer;
- automatic folder deletion semantics before contained data handling has a
  clear user-visible transaction; or
- enforcement of an encryption rule before its threat model and key handling
  are designed.

## Ownership and lifecycle

The component ownership is intentionally separated so that note-manager UI
does not become the source of truth:

    LocalDataKeyStore
            |
            v
    FileFolderCatalogStore <-> FolderCatalogManager
            |                        |
            |                        +--> FolderNotesModel / folder picker
            v
    FolderCatalog snapshot <---------- NotesWorkspaceController
            ^                                  |
            |                                  v
    storage folder import / change jobs <--- NoteStorage plugins

FolderCatalog is a validated value object. FileFolderCatalogStore is an
encrypted atomic file implementation. FolderCatalogManager, introduced after
the core persistence stage, owns the live catalog, coalesces durable writes,
publishes change signals, and performs recovery. It is initialized with the
same locally protected master key as drafts, but a corrupt folder catalog does
not make the application unusable:

1. It tries the primary encrypted catalog.
2. On failure it exposes Restore backup, Recreate catalog, and Quit choices.
3. Restore validates the backup before replacing the primary and preserves the
   unreadable files with a timestamped recovery suffix.
4. Recreate preserves both unreadable primary and backup, then starts with an
   empty catalog. Every note is initially Unsorted until a provider import or
   a user assignment repopulates membership.

This recovery flow is separate from draft recovery. Losing folder organization
is undesirable, but it must never block editing or weaken the existing
crash-safe draft guarantee.

FolderCatalogManager is the only component allowed to mutate the global tree
or local overlay. Storage jobs submit imported snapshots or assignment
acknowledgements to it; models and QML consume immutable projections. This
avoids competing storage refreshes overwriting a local drag operation.

## Core data model

### Global catalog

FolderCatalog is an application-owned, crash-safe catalog. It stores:

- FolderRecord: UUID, parent UUID, display name, sibling order, collapsed
  state, favorite flag, archived flag, revision and tombstone metadata;
- local note assignments: (storageId, noteId) -> folder UUID;
- provenance/synchronization metadata for assignments that a storage also
  persists; and
- stable path hints for providers that expose only a folder path.

Folder UUIDs are generated by QtNote. Folder names are unique among siblings;
the UI resolves a duplicate create or rename before persistence so path-based
providers have deterministic mappings. The root has no UUID.

The catalog is encrypted with the existing local secure-storage mechanism,
written through QSaveFile, and retains a backup of the last valid payload.
Failure to read it must never prevent QtNote from starting: the user can
restore the backup or recreate the catalog, in which case notes appear in
Unsorted until assignments are rediscovered or recreated.

### Note and draft metadata

Note gets a first-class nullable folderId; it is not hidden in backendData.
DraftRecord stores the intended folder separately from the source storage
identity. This matters because a draft can retain a pending folder change (and
later a pending target storage) while its text is unchanged.

NoteEditor distinguishes content dirtiness from metadata dirtiness. A
folder-only change must be checkpointed and published even if the text document
is clean. Publishing compares title, text, media, tags **and folder metadata**,
then uses the appropriate metadata-only storage operation where possible.

### Catalog merge

Providers with stable folder UUIDs, initially PTF and XMPP, import their
records directly. The catalog merges equal UUIDs field by field using
revision/tombstone ordering and imports their assignments.

Providers that only expose paths, initially Nextcloud, are reconciled by a
normalized ancestor path. A matching path reuses the existing global folder;
a new path creates the missing branch. The locally persisted path hint makes
this deterministic across restarts. Name collisions under different parents
are distinct. If external changes make a path ambiguous, QtNote preserves both
records and presents a repairable conflict instead of silently moving notes.

Tomboy imports no folders; it only receives local overlay assignments.

### Catalog payload and invariants

The encrypted catalog payload is versioned independently of the surrounding
secure envelope. Its first schema contains the following logical tables:

| Record | Required fields | Notes |
| --- | --- | --- |
| FolderRecord | id, parentId, name, sortOrder, collapsed, favorite, archived, revision, modifiedAt, tombstone | a null parentId is the root |
| NoteFolderAssignment | storageId, noteId, folderId, revision, modifiedAt, tombstone | a tombstone means explicitly Unsorted and wins over an older remote assignment |
| ProviderPathHint | provider instance, normalized path, folderId, revision | used when a provider only exposes a path, not a folder UUID |
| SyncState | provider instance, object identity, acknowledged revision, last error/retry time | keeps local optimistic operations distinguishable from confirmed remote state |

The initial core implementation starts with FolderRecord and
NoteFolderAssignment. Path hints and sync state are added with the first
provider adapter; their schema additions must have backward readers.

Catalog invariants are enforced before every in-memory replacement and every
durable write:

- every folder has a non-null UUID;
- every active folder has a non-empty trimmed name;
- active siblings have unique case-folded names;
- every active parent exists and is active;
- parent links cannot form a cycle;
- each storageId plus noteId pair occurs once;
- every active assignment points at an active folder; and
- tombstones retain enough identity/revision information to prevent an older
  provider refresh from resurrecting a removed assignment.

Folder renames and moves increment their local revision and timestamp. Imported
records with a higher revision win. Equal revisions use a deterministic
timestamp tie-break; a true equal-revision divergence is surfaced as a
conflict, never silently merged field by field. A local UI operation has an
optimistic revision and remains visibly pending until its provider acknowledges
it or reports failure.

### Tree merge algorithm

Merging is not a destructive import. For every storage refresh:

1. Validate the imported records as a standalone snapshot.
2. Match stable UUID records to catalog records.
3. Apply only winning revisions, retaining tombstones.
4. For path-only records, normalize path separators and names, then resolve
   each ancestor in order through ProviderPathHint.
5. Create missing global folders only after their parent path is resolved.
6. Merge assignments after folders, so an assignment never temporarily points
   to a missing node.
7. Emit one catalog transaction and persist it once.

PTF and XMPP have stable UUID records. Nextcloud supplies a category path, so
it uses the path branch of this algorithm. Tomboy does not participate in tree
import. A collision that cannot be resolved unambiguously remains a catalog
conflict with both data sources preserved; it is not repaired by choosing a
random sibling or moving a note.

### Assignment state transitions

The desired folder is attached to the editing session immediately. The
following paths are then possible:

| Initial note state | User action | Durable action |
| --- | --- | --- |
| clean, storage supports folders | drag or picker | optimistic catalog update, then metadata-only changeNoteFolder job |
| clean, storage has no folder support | drag or picker | encrypted local overlay only |
| dirty editing note | drag or picker | checkpoint desired folder into its draft; publish applies it with content |
| offline or failed provider | drag or picker | retain pending overlay and retry according to storage retry policy |
| cleared to Unsorted | drop on Unsorted or picker | tombstone/clear assignment; issue provider clear when supported |

The source storage and target publication storage are distinct draft fields.
This prevents a folder rule from accidentally changing where an existing
remote note is read from, and prevents a target-storage rule from losing the
folder selected while the note was offline.

## Storage contracts and backends

NoteStorage gains an asynchronous changeNoteFolder operation and capability
reporting. The operation is deliberately narrow: it changes only folder
metadata and uses the provider's concurrency token. It enables an unchanged
note to move without rewriting its body. A storage that cannot persist folders
reports that fact; the catalog overlay still gives the user a working folder
assignment.

| Backend | Folder persistence | Initial implementation |
| --- | --- | --- |
| Draft store | encrypted draft payload | persist pending folder metadata with every checkpoint |
| Remote cache | encrypted cache record | cache folderId and synchronization state |
| PTF | .qtnote-folders.json beside the PTF notes | atomic catalog/index; preserve current note IDs and media layout rather than moving files |
| XMPP | encrypted index/catalog records | publish folder ID/path and catalog changes; no migration from the previous XMPP format is required |
| Nextcloud | Notes API category path | map tree paths to category paths and use a metadata-only conditional update; empty folders and folder flags remain local |
| Tomboy | none in Tomboy data | encrypted local overlay only; never alter XML tags for folders |
| Other plugins | capability fallback | overlay until their specifications are reviewed |

For a clean note, a move invokes changeNoteFolder. For a dirty note, the new
folder is recorded in its editing draft and is applied during publication. If a
backend request fails, the catalog retains a visibly pending assignment and
offers retry; it does not pretend that the remote move succeeded.

### Storage API contract

The public storage contract needs a capability bit for folder metadata and a
dedicated asynchronous job. The job receives:

- the stable storage note identity;
- the desired nullable folder UUID and provider path representation;
- the provider's expected concurrency token, such as an ETag or revision;
- a request identity used to reconcile an optimistic catalog mutation; and
- cancellation and normal StorageJob error semantics.

The result returns the refreshed note metadata and concurrency token. It must
not rewrite a note body, title, tags, media, or unrelated storage metadata.
An unsupported provider completes with a clear capability result rather than
pretending the overlay was remotely saved.

Folder tree synchronization is a separate capability from per-note assignment:
some providers can carry a category path for notes but cannot represent an
empty folder, a favorite folder, or an archived folder. In that case the
catalog remains authoritative for tree-only state and only the representable
per-note assignment is exported.

The storage job layer receives a specific folder-change job type rather than
overloading a full save job. NotesIndex and NotesWorkspaceController use its
completion to update the summary, retry status, and active editor metadata.

### Draft, cache and Note migration details

The following serialized formats need a version bump with backward readers:

| Payload | Existing role | Folder addition |
| --- | --- | --- |
| DraftRecord | crash-safe editing/publish queue | intended folderId and target storage fields; folder-only changes increment checkpoint revision |
| FileDraftStore | encrypted individual draft payload | reader accepts all current pre-folder versions and initializes folder to null |
| RemoteCacheRecord | encrypted remote offline snapshot | cached folderId plus pending folder synchronization state |
| FileRemoteCacheStore | encrypted per-provider cache | reader accepts the previous payload version and initializes null folder |
| Note shared data | live model/editor value | nullable folderId with copy/equality/debug handling |

DraftManager needs a metadata-dirty path. Its present document dirty flag is
not sufficient because a user can assign a folder without changing text.
Checkpoints, publication equality checks, recovery, duplicate/copy conflict
resolution, and deferred routing must all preserve this metadata. A recovered
draft restores its desired folder before the editor is presented.

### PTF implementation detail

PTF keeps its existing root-level note files and media sidecars. Moving files
into directories would change note identities, break media paths, complicate
external file handling and make rollback much harder. Instead, the PTF root
gets a hidden atomic index file named .qtnote-folders.json:

- versioned JSON contains the PTF-provided folder records, assignments, and
  locally generated UUIDs;
- writes use QSaveFile and retain a valid backup;
- a missing index means every PTF note is Unsorted;
- a malformed index is recoverable without scanning or rewriting note bodies;
- a note rename updates the assignment key in the same catalog transaction;
  until durable completion the old identity remains recoverable; and
- an external PTF note file is still discovered normally and receives no
  folder until an assignment is known.

The PTF provider imports this index into the global catalog on initialization
and refresh. changeNoteFolder updates only the PTF index, emits the normal
note-modified/index signals, and does not touch note file mtime solely for a
folder operation. PTF folders are therefore portable within a PTF directory,
but their state does not need to leak into individual Markdown or text files.

### XMPP implementation detail

XMPP gets a new encrypted catalog/index representation containing global
folder UUIDs, parent relations, relevant flags, revisions, assignments, and
path snapshots. Folder-only updates receive their own revision so changing a
folder does not require uploading note content again.

There is intentionally no migration from the previous XMPP folder-less
protocol: this product does not require backwards compatibility for XMPP.
The new codec must nevertheless reject mixed or malformed versions safely,
bind every encrypted payload to its account/node context, and preserve
concurrent remote updates as conflicts rather than dropping either revision.
The remote cache reflects the newly decoded folder metadata before the manager
model is updated.

### Nextcloud implementation detail

Nextcloud Notes exposes a category string that can encode a slash-separated
folder path. QtNote maps a global folder's visible ancestor path to that
category. The provider performs a conditional metadata-only update with the
current ETag, then refreshes cache metadata from the response.

Nextcloud cannot independently represent an empty folder or global folder
flags. Such folders, collapse state, favorites and archived state remain in
FolderCatalog. If an externally changed category path maps to an existing
normalized path it merges; otherwise it creates/imports the required branch
through path hints. A Nextcloud note favorite remains a note favorite and must
not be confused with the QtNote folder favorite attribute.

### Tomboy and unsupported providers

Tomboy changeNoteFolder is logically supported through the encrypted local
overlay but reports no native persistence capability. It never adds a tag,
notebook marker, XML element or filename convention. Removing a Tomboy
storage leaves its overlay records recoverable until the user removes the
storage's local data; reconnecting the same stable storage/note identity
restores them.

Any other provider begins in the same overlay-only mode. It may opt into
native assignment or tree import only after its own format and concurrency
rules have been reviewed and tested.

## Notes manager UX

NotesManagerPage.qml gains a third projection, **Folders**, alongside Recent
and By storage.

- The compact toolbar contains Add note, Add folder and Collapse all.
- The tree contains real folders plus the virtual Unsorted section.
- Folder and note selection supports the existing desktop multi-selection
  behavior; a multi-note drag presents a compact consecutive drag preview even
  when source rows were non-consecutive.
- Folders can be moved/reparented; notes can be dropped into folders, to
  Unsorted, or between visible note rows according to the resulting folder.
- Collapsed folders do not open just because the pointer pauses over them.
- Right-click context menus are isolated from left-click activation and remain
  valid on both X11 and Wayland.

The task-list hierarchy currently has its own subtree/depth and target
calculation. The reusable reorder toolkit will be extended with tree
structure/geometry and auto-scroll primitives, while domains keep their own
model mutations and delegate visuals. This avoids forcing folder, task and
settings lists into a single incompatible delegate while eliminating duplicated
drag mathematics.

### Folder tab interaction contract

The toolbar has exactly three compact actions:

1. Add note. It creates a note in the currently selected real folder when
   there is one; otherwise its initial folder is Unsorted. The editor's folder
   picker remains available before publication.
2. Add folder. It creates a root folder unless invoked from a selected folder's
   context menu, in which case it creates a child and starts inline rename.
3. Collapse all. It records the collapsed state of every real folder in one
   catalog transaction. It does not affect Unsorted.

Unsorted is displayed as a durable-looking but virtual root section. It
contains every visible note with no active assignment, including notes whose
remote assignment is pending a clear operation. It cannot be moved, renamed,
archived, favorited, collapsed, or deleted.

Folder context menus contain only meaningful structural and state actions:
create subfolder, rename, favorite/unfavorite, archive/unarchive, collapse or
expand, and future deletion once its transaction is specified. There is no
Move to root command. The normal way to get to the root is drag and drop.

Folder rows show a disclosure control, a folder icon, inline-editable name,
and compact state indicators for favorite/archive/pending error when needed.
They do not use task checkboxes. Note rows retain storage identity/icon
information so a mixed-storage folder remains understandable.

### Selection, activation and drag/drop

Desktop follows the note-manager selection convention:

- left click selects and opens a note in the right pane;
- Ctrl or Command click adds/removes an item from the selection;
- Shift selects a contiguous visible range;
- right click selects the target only when it was not already selected, then
  opens a context menu without allowing that click to activate the row below;
- keyboard arrows, Space, Enter, Delete and context-menu key have equivalent
  behavior; and
- on mobile, long press enters selection/reorder mode instead of relying on
  modifier keys.

A multi-note drag uses the selected note set. The temporary drag projection is
compact: it displays selected notes as adjacent drag rows rather than preserving
the holes left by unselected source rows. This is a visual projection only;
the underlying folders and ordering are changed once at drop completion.

For a folder drag, an insertion target is valid only when it is not inside the
dragged folder's subtree. Horizontal depth gestures select a valid parent,
subject to the parent being visible and not archived only if the UI allows
management of archived folders. Dropping onto a folder makes it the parent or
assignment target. Dropping between rows changes sibling order. Dropping on
Unsorted clears a note assignment; a folder cannot be dropped there.

A collapsed folder remains collapsed throughout drag. Hover does not cause an
automatic timer-based expansion. A user who wants to target a child explicitly
opens it before dragging; a direct drop on the closed folder is still accepted
as a drop into that folder.

### Inline rename safety

The delegate holds a staged name while edit mode is active. Enter and focus
loss request a catalog transaction after local validation; Escape restores the
previous name. Focus loss caused by a context menu or drag does not discard a
valid name. The row stays in a pending/error state if durable persistence
fails, with Retry and Cancel/Revert actions. A successful write emits one
model update and does not recreate the delegate, preserving keyboard focus.

### Editor folder picker

The editor toolbar/menu contains a compact folder picker. Its order is:

1. Unsorted;
2. favorite folders and their required ancestor context;
3. remaining non-archived tree in sibling order; and
4. the note's currently assigned archived folder, marked archived, if needed.

The picker never silently changes storage. Choosing a folder on a clean note
starts the metadata job; choosing it on a dirty note checkpoints it as draft
metadata. If a move is pending it indicates that fact and prevents duplicate
requests while still allowing an explicit change to a different folder.

### Reorder toolkit extraction

The existing reusable linear components remain responsible for generic drag
preview, pointer capture and displacement animation. The task-list code and
new folder model share the following extracted primitives:

| Primitive | Responsibility | Domain-specific input |
| --- | --- | --- |
| TreeProjection | flatten a hierarchy into visible rows, subtree bounds and depths | folder/task model accessors |
| HierarchicalDropTarget | calculate before/inside/after target and allowed depth | parent validity and indentation policy |
| ReorderAutoScroll | scroll viewport while a drag is near an edge | Flickable/ScrollView geometry |
| CompactDragProjection | arrange selected non-consecutive rows without holes | selected row identities |
| ReorderTransaction | begin, update, commit or cancel an optimistic move | domain mutation and persistence callback |

Folder, task and settings delegates keep their own icons, toggles, context
menus and model mutations. The toolkit owns geometry and input timing, not
business semantics. This is why simply substituting AnimatedSettingsList for a
note manager list would be incorrect: settings is linear and owned by QWidget
settings state, while folders/tasks are expandable trees with different
transaction targets.

## Rules

The settings dialog gets a QML Rules page backed by a UI-neutral
RulesController and persistent NoteRule records.

Initial conditions:

- title matches a pattern;
- note has a tag;
- note text matches a pattern; and
- storage matches.

Initial actions:

- assign a folder; and
- select the target storage for publication.

Encryption is represented as a future action in the model/UI contract but is
not enforced until its security policy is designed and implemented.

Rules execute after a note is read/imported and before a new or modified note
is published. Ordered rules use explicit stop/continue semantics, are
idempotent, and record their last applied result to prevent refresh loops.
Text conditions may require an asynchronous full-note load; summary-only list
refreshes must not block on them. Applying a Tomboy rule changes only the
local overlay assignment.

### Rule data model and editor

A NoteRule record has a UUID, enabled flag, ordered position, name, condition
combiner, condition list, action list, stop-processing flag, revision, and
last-result marker. Rules are stored in encrypted local application settings
and edited through a QML page backed by RulesController rather than directly by
widgets.

The first editor supports an explicit ALL or ANY combiner for these
conditions:

- title matches a case-sensitive or case-insensitive pattern;
- any tag equals or matches a pattern;
- body text matches a pattern;
- source storage matches one or more storage IDs; and
- optional negation of each condition.

The first actions are Set folder and Set publication storage. More than one
folder action in the same rule is invalid; a later matching rule may override
an earlier one only when the earlier rule did not stop processing. The UI
explains this order rather than hiding it behind unspecified priority.

Rules are re-orderable with the same linear reorder toolkit used by settings.
Each rule has a dry-run preview against a selected note, validation errors
before Apply, and a human-readable summary. Folder targets show archived state
and do not disappear when an old rule refers to an archived folder.

### Rule execution model

There are two execution phases:

1. Import phase runs folder-safe summary conditions after a note is read from a
   storage. Text conditions queue a low-priority full-note evaluation instead
   of blocking list loading.
2. Publication phase evaluates the complete rule set against the draft/live
   note before routing and save. It can set the intended folder and target
   storage atomically in the draft metadata transaction.

The evaluator receives an origin marker containing rule UUID, input
revision/hash, and outcome. Re-evaluating the same unchanged note/rule pair is
a no-op. A rule-generated storage change cannot make the import phase bounce a
note endlessly between storages; such a cycle is detected and reported as a
rule error. Rules never overwrite a direct user folder choice made later in
the same editing session unless the user explicitly asks to reapply rules.

Text matching must have resource limits: a cancellable asynchronous load,
bounded pattern execution, and no body content leaked to logs. A failed full
load marks the text condition indeterminate and retries according to ordinary
storage availability rather than treating it as a match.

## Format compatibility, recovery and privacy

Existing drafts and remote caches remain readable. New fields default to an
empty folder, which means Unsorted. Payload version readers must reject
trailing garbage, duplicate records, invalid UUIDs, impossible parent links,
and malformed media as they do today. A newer unknown payload version reports
a clear recovery error and preserves the original file; it never writes an
empty replacement automatically.

The global encrypted catalog uses a dedicated SecureEnvelope key domain and
authenticated context. Its primary and backup files are owner-readable only.
PTF's local index follows the existing PTF privacy model; it is not made
mistakenly encrypted independently of the note files. XMPP's folder payload is
encrypted in the account-bound protocol layer. No folder name, title, tag,
body, key material, or rule text is logged in diagnostics.

The application startup recovery UI will clearly separate:

- unreadable crash-recovery drafts, which are safety-critical for editing;
- unreadable global folder catalog, which permits a safe Unsorted startup; and
- a provider-specific PTF/XMPP index issue, which permits the storage to load
  with folder state unavailable while preserving its original index.

## Detailed test strategy

Every stage supplies focused automated tests before its commit:

| Area | Required coverage |
| --- | --- |
| FolderCatalog | sibling-name validation, cycle rejection, move/reorder, flags, assignment tombstones, merge revisions and conflict preservation |
| FileFolderCatalogStore | encrypted round trip, no plaintext leakage, atomic backup, corrupt primary restore, recreate preservation, wrong key and tampering |
| Note/drafts/cache | old payload read, new field round trip, folder-only checkpoint/publication, dirty content plus folder, recovery and conflict-copy behavior |
| PTF | absent/corrupt index, folder index round trip, external note discovery, note ID rename, metadata-only move and recovery backup |
| XMPP | codec round trip, encrypted context binding, malformed record rejection, revision conflict and cache synchronization |
| Nextcloud | category path conversion, ETag request semantics, remote path import and conflict behavior |
| Tomboy | no XML/tag mutation from folder assignment, overlay persistence, rule-assigned folders and reconnect behavior |
| Manager QML | Folders tab, Unsorted, inline rename, collapse all, picker ordering, selection, multi-drag compact preview, no hover expansion and context-menu click isolation |
| Rules | validation, ALL/ANY/negation, ordering/stop behavior, storage/folder routing, idempotence and text-load cancellation |

QML tests must use an explicit offscreen/software configuration. Tests that
exercise delete or recovery paths use a fake dialog/controller response and
must never display a real destructive application dialog during a normal build.

## Implementation stages and dependencies

The dependency chain is:

1. Design document
2. FolderCatalog and encrypted local store
3. Note, draft and remote-cache metadata
4. Storage API and PTF implementation
5. XMPP, Nextcloud and Tomboy capability paths
6. Folder model, picker and manager tab
7. Rules settings and execution
8. Integration tests and migration/recovery checks

The catalog is required before metadata and UI. Metadata is required before a
storage move and editor picker can be correct. Storage adapters and the manager
can then proceed in parallel where their interfaces are stable; rules depend
on both the catalog and the editor/publication routing.

0. **Design record** — this document. It is committed before behavior changes.
1. **Catalog core** — introduce folder records, assignment keys, encrypted
   atomic persistence, recovery behavior and unit tests. No UI or provider
   behavior depends on it yet.
2. **Metadata propagation** — add folderId to Note, draft payloads and
   remote-cache payloads; bump formats with backward readers; make folder-only
   draft changes publishable.
3. **Storage contract and PTF** — add jobs/capabilities, PTF index merge and
   metadata-only moves, with file/recovery tests.
4. **Backend adapters** — XMPP's new encrypted catalog/index, Nextcloud path
   mapping/conditional metadata update, and the Tomboy overlay-only adapter.
   Each provider gets capability and conflict tests.
5. **Manager and editor UI** — folder projection, picker, inline rename,
   hierarchy drag/drop and shared reorder extraction. QML tests run headless;
   no interactive destructive dialog is opened by automated tests.
6. **Rules** — schema/controller/QML page, deterministic evaluator and
   folder/storage actions. Add tests for matching, ordering and Tomboy overlay
   behavior.
7. **End-to-end hardening** — mixed-storage tree merge, drafts, offline
   retries, corrupted catalog recovery, accessibility/keyboard navigation and
   full build/test verification.

Every completed stage receives its own local commit. Later stages may start
only after their listed dependency builds and its focused tests pass.

### File-level delivery map

The following map keeps the implementation reviewable and prevents a
folder-specific shortcut from becoming scattered backend data:

| Area | Primary files/classes |
| --- | --- |
| catalog value/persistence | foldercatalog, filefoldercatalogstore, secureenvelope key domain |
| live catalog lifecycle | new FolderCatalogManager and application initialization/recovery integration |
| note metadata | note, notedata, noteeditor, draftmanager, draftstore, filedraftstore |
| remote metadata cache | remotecachestore, fileremotecachestore and remote provider adapters |
| storage API | notestorage, storagejob, NotesWorkspaceController, NotesIndex |
| PTF | ptfstorage and focused PTF tests |
| XMPP | xmppdto, xmppnotecodec, xmppstorage and codec tests |
| Nextcloud | Nextcloud DTO/worker/storage and request tests |
| Tomboy | tomboystorage only for overlay coordination; no XML folder fields |
| manager UI | folder model/controller projection, NotesManagerPage and new folder delegates |
| editor picker | editor toolbar/menu QML plus NotesWorkspaceController operations |
| reorder toolkit | libqtnote/qml/reorder tree primitives and task-list migration |
| rule settings | RulesController, persistent rule store, Settings QML and rule evaluator |

### Stage acceptance gates

0. Documentation is complete enough that storage, model and QML work can use
   the same terminology and ownership decisions.
1. Catalog core builds in the core-only configuration and its unit test proves
   validated hierarchy, encryption, backup restore and explicit recreation.
2. Existing draft/cache tests plus new migration tests pass; opening an old
   draft keeps it Unsorted and a metadata-only change survives restart.
3. PTF tests prove that a folder move never changes note content or media
   layout and that a damaged PTF index is recoverable.
4. Every enabled remote provider builds with its capability path. The absence
   of the optional XMPP dependencies must not reduce core/other-provider
   coverage.
5. The Folders tab works under desktop and mobile width constraints; all
   interactions have an offscreen QML test and no regression to Recent or By
   storage selection/menu behavior.
6. Rules have deterministic tests and cannot create a publication loop.
7. A clean full build, CTest run, manual desktop smoke test and a structured
   report document all completed work, intentionally deferred pieces, risks
   and follow-up design decisions.

## Open design checkpoints

- Decide the exact visible UI for a catalog conflict or failed remote folder
  move before exposing XMPP/Nextcloud synchronization broadly.
- Confirm whether a folder can be deleted only after explicit rehoming of its
  contained notes/subfolders, or whether deletion should offer an atomic
  “move contents to Unsorted” operation.
- Decide whether folder-specific styling or counts are needed before the
  manager's mobile layout is finalized.
- Define encryption policy and key-availability behavior before enabling the
  planned encryption rule action.
