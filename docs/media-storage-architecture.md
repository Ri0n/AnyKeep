# Media storage architecture

Status: partially implemented. The shared encrypted local blob store, draft/cache
manifests, `anykeep-media:` references, image/audio insertion and playback, and
PTF sidecars are in place. Remote media transport and Tomboy/Gnote adapters
remain future work.

AnyKeep notes are Markdown documents. Attachments and inline media are addressed
from Markdown by a stable attachment UUID and a readable portable filename:

```markdown
[Project budget.xlsx](anykeep-media:/7f28c5de-5f48-4d44-918f-b24d5b672f30/project-budget.xlsx)
![Wiring diagram](anykeep-media:/d839a73b-9818-41aa-a939-59d50fce94fb/IMG_142315.png)
<audio controls src="anykeep-media:/9b82323d-6aa4-43d1-b547-acde11bd37fe/Audio_20260801_211300.m4a" title="Audio recording" data-anykeep-duration-ms="42000"></audio>
```

The UUID is authoritative. The final URI component is only a readable fallback
for raw Markdown, recovery, and export. Link text, image alt text, and the audio
`title` attribute are user-visible descriptions. The attachment manifest retains
the original filename. Audio duration is structural editor metadata; the player
also accepts the duration reported by the decoder at runtime.

## Design goals

- render useful Markdown both in rendered and raw-source modes;
- keep drafts and remote caches encrypted at rest;
- avoid copying large attachments when a note moves between a cache and a draft;
- support rerouting a failed publication to another storage;
- preserve original filenames without using unsafe names on disk;
- deduplicate immutable local content;
- keep storage-specific transports behind backend interfaces;
- support multiple configured storage instances per plugin.

## Hybrid storage model

AnyKeep uses logically separate draft and remote-cache records, but they refer to
one profile-wide immutable media store. Each record owns its manifest; the shared
store owns only encrypted bytes.

```mermaid
flowchart LR
    subgraph Records["Logical owners"]
        D1["Draft record"]
        D2["Another draft"]
        XC["XMPP cache instance"]
        NC["Nextcloud cache instance"]
    end

    subgraph Media["Profile-wide LocalMediaStore"]
        B1["Encrypted immutable blob A"]
        B2["Encrypted immutable blob B"]
        B3["Encrypted immutable blob C"]
    end

    D1 -->|"manifest reference"| B1
    D1 -->|"manifest reference"| B2
    D2 -->|"manifest reference"| B1
    XC -->|"manifest reference"| B1
    XC -->|"manifest reference"| B3
    NC -->|"manifest reference"| B2
```

This is hybrid rather than fully global storage: physical bytes are shared, while
ownership, synchronization state, original names, and remote metadata remain in
the draft or cache record that uses them.

An attachment blob is immutable. Editing an image or replacing a document creates
a new blob and updates the manifest. Existing notes and recoverable drafts continue
to reference the old content.

## Data model

The exact C++ API may evolve, but the persistent model should contain the following
information:

```cpp
struct MediaReference {
    QUuid id;                 // Attachment identity used by anykeep-media URIs.
    QByteArray blobId;        // Identity of immutable local content.
    QString originalName;     // Original display/export filename.
    QString portableName;     // Cross-platform fallback used in the URI.
    QString mediaType;        // Validated MIME type.
    qint64 size = 0;
    QByteArray checksum;      // Plaintext integrity metadata.
    QVariantMap remoteData;   // Backend-specific source/object metadata.
};
```

`DraftRecord` and `RemoteCacheRecord` each persist a list of `MediaReference`
objects. A media reference is not a filesystem path and does not contain encrypted
blob bytes.

```mermaid
classDiagram
    class DraftRecord {
        +UUID id
        +String body
        +PublicationState state
        +List~MediaReference~ media
    }

    class RemoteCacheRecord {
        +String id
        +String body
        +SyncState state
        +Map backendData
        +List~MediaReference~ media
    }

    class MediaReference {
        +UUID id
        +ByteArray blobId
        +String originalName
        +String portableName
        +String mediaType
        +Integer size
        +ByteArray checksum
        +Map remoteData
    }

    class LocalMediaStore {
        +put(stream) blobId
        +open(blobId) stream
        +contains(blobId) bool
        +sweep(liveBlobIds)
    }

    DraftRecord "1" *-- "0..*" MediaReference
    RemoteCacheRecord "1" *-- "0..*" MediaReference
    MediaReference "0..*" --> "1" LocalMediaStore : blobId
```

## Blob identity and layout

The preferred blob identifier is a keyed content digest:

```text
blobId = HMAC-SHA-256(derived media-ID key, plaintext bytes)
```

It permits local deduplication without exposing a directly testable ordinary
content hash in filenames. The encrypted envelope still records and authenticates
the expected identifier, size, schema, and object kind.

A sharded physical layout avoids oversized directories:

```text
<AnyKeep data>/media/
  ab/
    cd/
      abcdef...blob
```

Blob files never use the original filename. Original and portable filenames are
encrypted manifest metadata.

## Encryption domains

Drafts, remote caches, and shared media use the same profile-local master key from
the operating-system keychain, but different derived encryption domains:

```mermaid
flowchart TD
    MK["Profile-local master key<br/>OS keychain"]
    KD["LocalDraft derived key"]
    KC["LocalRemoteCache derived key"]
    KM["LocalMedia derived key"]
    DR["Encrypted draft records"]
    CR["Encrypted cache snapshots"]
    MB["Encrypted immutable media blobs"]

    MK --> KD --> DR
    MK --> KC --> CR
    MK --> KM --> MB
```

All three use the same authenticated-encryption implementation. Domain separation
prevents a cache record, draft record, and media blob from being substituted for
one another. Media has one shared domain because every manifest refers to the same
physical `LocalMediaStore` object.

## Editing, publication, and rerouting

The note-level state machine, multi-editor lease rules, publication retries, and
conflict handling are defined in
[Note lifecycle architecture](note-lifecycle-architecture.md).

Blob bytes are written before the new manifest is committed. A crash can therefore
leave an unreferenced blob, which is safe and can be collected later. A manifest
must never reference a blob that has not been durably committed.

```mermaid
sequenceDiagram
    participant U as Editor
    participant M as LocalMediaStore
    participant D as DraftStore
    participant R as Remote backend
    participant C as RemoteCacheStore

    U->>M: Put attachment plaintext stream
    M-->>U: Durable immutable blobId
    U->>D: Save body and media manifest
    U->>R: Publish note and attachment

    alt publication succeeds
        R-->>U: Confirm remote IDs and revisions
        U->>C: Save confirmed body and manifest
        U->>D: Remove published draft
    else publication fails or is rerouted
        R-->>U: Error
        Note over D,M: Draft and local blob remain recoverable
        U->>R: Retry through selected storage backend
    end
```

Creating a draft from a cached note copies the body and manifest metadata, but not
the media bytes. Successful publication similarly installs references in the cache
without copying local blobs. Publishing to a remote service necessarily decrypts
the local blob stream and transforms or encrypts it for that service; this does not
modify the local blob.

## Garbage collection

Persistent reference counters are deliberately avoided because a crash between a
manifest update and a counter update could make them incorrect. Collection uses a
mark-and-sweep pass:

```mermaid
flowchart TD
    START["Start collection"] --> D["Read all draft manifests"]
    D --> C["Read every remote-cache instance manifest"]
    C --> O["Include open editor sessions and pending commits"]
    O --> LIVE["Build set of live blob IDs"]
    LIVE --> SCAN["Scan LocalMediaStore"]
    SCAN --> AGE{"Unreferenced and older<br/>than grace period?"}
    AGE -->|No| KEEP["Keep blob"]
    AGE -->|Yes| DELETE["Delete blob"]
```

The grace period protects interrupted imports, publication rerouting, rollback, and
older cache snapshots. Collection must skip manifests it cannot authenticate or
read; unreadable ownership information must never be interpreted as an empty set.

## Filenames and export

`originalName` is preserved as metadata after Unicode validation and a length
limit. It is never trusted as a path. `portableName` is generated for the Markdown
URI and filesystem materialization by:

- removing control characters and path separators;
- replacing Windows-reserved characters;
- rejecting `.` and `..`;
- handling reserved device names such as `CON`, `PRN`, `AUX`, and `NUL`;
- removing trailing spaces and periods;
- bounding the encoded filename length while preserving the extension;
- resolving collisions with deterministic numeric suffixes.

PTF, Tomboy, and similar file-oriented stores may materialize attachments in a
sidecar directory named after the note. This is a storage adapter representation,
not AnyKeep's internal ownership model:

```text
note.ptf
note/
  project-budget.xlsx
  IMG_142315.png
```

The adapter converts `anykeep-media:` URIs to relative paths on export and performs
the inverse import into `LocalMediaStore` on load.

## Audio recording and playback

AnyKeep requests AAC-LC in an M4A/MP4 container, mono, 48 kHz, 64 kbit/s average
bit rate. The recorder is exposed only when the active Qt Multimedia backend can
resolve that codec/container pair; it does not silently fall back to a
platform-specific container.

Native encoders require a seekable output file. Recording therefore uses a
short-lived owner-only temporary directory, imports the finished encoded bytes
into `LocalMediaStore`, and removes the temporary plaintext file immediately
after import. Normal playback decrypts the immutable blob into a seekable
`QBuffer` and supplies it to `QMediaPlayer`; no persistent plaintext playback
copy is created.

Audio is a first-class structural block. Its `anykeep-media:` URI participates in
manifest pruning, undo/redo, internal copy/paste, cross-note attachment cloning,
and PTF sidecar import/export exactly like an image URI.

## Remote backend boundary

The local media model is independent of the remote transport. Each remote storage
adapter maps a `MediaReference` and local plaintext stream to its native object and
stores the resulting remote metadata in its own manifest.

```mermaid
flowchart LR
    MAN["MediaReference"] --> API["Remote media backend boundary"]
    LOCAL["LocalMediaStore stream"] --> API
    API --> XMPP["XMPP candidate:<br/>XEP-0447 metadata and sources"]
    API --> NC["Nextcloud object/file"]
    API --> OTHER["Other storage-specific transport"]
```

### XMPP direction

[XEP-0447: Stateless File Sharing](https://xmpp.org/extensions/xep-0447.html)
is the current candidate for representing XMPP attachments. It provides generic
file metadata, hashes for integrity and caching, inline/attachment disposition,
and one or more transport-independent sources. XEP-0447 does not itself define
byte storage or transport, but a source can advertise a published Jingle session.
The receiver can then request the content using
[XEP-0234: Jingle File Transfer](https://xmpp.org/extensions/xep-0234.html)
and negotiate an appropriate Jingle transport.

```mermaid
flowchart TD
    SFS["XEP-0447 file-sharing<br/>metadata, hashes, disposition"]
    SRC{"Advertised source"}
    JP["Jingle-published source"]
    URL["URL source"]
    JFT["XEP-0234 Jingle File Transfer"]
    DC["XEP-0343 WebRTC DataChannel<br/>SCTP over DTLS"]
    JET["XEP-0391 encrypted<br/>Jingle transport"]
    JHTTP["XEP-0370 Jingle HTTP<br/>HTTP or HTTPS"]
    HFU["XEP-0363 HTTP File Upload<br/>persistent source"]

    SFS --> SRC
    SRC --> JP --> JFT
    SRC --> URL --> HFU
    JFT --> DC
    JFT --> JET
    JFT --> JHTTP
```

Jingle therefore does not imply one fixed byte channel. A WebRTC DataChannel
transport uses SCTP over DTLS and protects the transferred media. Jingle Encrypted
Transports can add an end-to-end encryption layer around a negotiated Jingle
transport, including a transport whose underlying exchange is HTTP. Jingle's HTTP
transport can itself use HTTPS. These layers must be recorded separately in the
protocol design: HTTPS protects a client-to-server hop, DTLS protects its channel,
and a Jingle end-to-end encryption layer protects the file stream between the
participating XMPP entities.

For persistent notes, source lifetime remains important. A direct DataChannel is
useful while another device is online, but is not by itself durable storage. An
HTTP-uploaded source is naturally available to offline devices, but AnyKeep must
decide whether it stores plaintext behind HTTPS or uploads an independently
encrypted object whose key is distributed through the authenticated XMPP storage
protocol. Multiple XEP-0447 sources may allow both paths for the same content.

The specification is Experimental, and its primary examples use messages,
Carbons, and MAM. Before implementation AnyKeep still needs to define how an
XEP-0447 file-sharing description and its source lifetime are bound to a persistent
note revision in the private PubSub protocol. The design must also decide:

- which source protocols AnyKeep requires, prefers, or advertises together;
- which Jingle transports and end-to-end encryption profiles are interoperable;
- how remote attachment encryption and key rotation work;
- whether sources survive as long as their note revisions;
- how replacement and deletion avoid breaking older revisions;
- how hashes map to the local keyed `blobId` without publishing the keyed value;
- which XEP-0446 metadata and thumbnails are retained in `MediaReference`.

Until those questions are specified, XEP-0447 is a direction and interoperability
target, not part of the implemented AnyKeep XMPP wire protocol.

## Failure and security rules

- Validate declared size, MIME type, decoded dimensions, and integrity digest.
- Enforce configurable per-file and aggregate limits before allocation or decode.
- Stream large files; do not require a complete plaintext copy in memory.
- Treat SVG, HTML, PDF, and active document formats as untrusted input.
- Never resolve attachment names as relative or absolute input paths.
- Do not automatically open downloaded attachments in external applications.
- Authenticate the encrypted envelope before exposing any plaintext.
- Keep the draft until both note and required attachment publication are confirmed.
- Do not delete remote media merely because one local reference disappeared; the
  remote backend must apply its revision and ownership rules.

## Implementation stages

1. Add `MediaReference`, URI parsing, filename normalization, and Markdown
   rendering hooks.
2. Implement encrypted immutable `LocalMediaStore` and its tests.
3. Add media manifests to draft and remote-cache payload versions.
4. Implement insertion, preview, export, and safe external opening.
5. Add mark-and-sweep collection with a grace period.
6. Implement PTF/Tomboy sidecar adapters.
7. Specify and implement the XMPP mapping, using XEP-0447 where appropriate.
8. Define transports independently for other remote-storage plugins.
