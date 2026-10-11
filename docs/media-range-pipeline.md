# Shared remote-media range pipeline (proposed)

Status: **design proposal, not implemented**. Base implementation: PR #155, squash commit `af3c0fe`.
See [media storage architecture](media-storage-architecture.md) for the persistent data model, security constraints,
and the current ranged playback design.

## Motivation

Every remote-media plugin should provide **a chunk source**, not implement a separate
playback download scheduler. A decoder can issue random byte ranges, abort them
during seeks, and concurrently ask for overlapping ranges. A single missing chunk
must be fetched at most once per immutable source while any number of consumers
wait for it. Already authenticated cache records are reused without network access.

Transport throughput, connection limits, cancellation semantics, and support for
out-of-order retrieval vary. The common layer should manage demand and bounded
parallelism **without reducing parallelism already implemented in a provider**.
WebDAV can fetch independent ranges concurrently; Iris/Jingle already overlaps
old and new content during BUNDLE range handover, may have multiple live media
syncs, and must preserve its negotiated transport and deferred-receipt rules.
These are not interchangeable HTTP connections: account for *actual active
network traffic*, not just the number of queued range requests.

## Current implementation (October 2026)

| Component | Existing responsibility | Missing for the target design |
| --- | --- | --- |
| `MediaRangeService` | Loopback HTTP server for `QMediaPlayer`: parses HTTP Range; asynchronously requests 64 KiB slices; throttles socket writes; only the first read of each GET may reprioritize retrieval | Cancellation propagated upstream on socket disconnect, explicit per-player demand/session tracking |
| `MediaSyncService` | Publishes and observes *durably verified* byte counts and download states | Intentionally not a scheduler; keep it separate |
| `LocalMediaStore` / `MediaPartialChunkCache` | Shared storage layer already exposes `LocalMediaStore::partialMediaCache()` for Nextcloud. The returned shared `MediaPartialChunkCache` stores locally sealed, reauthenticated 1 MiB records under `media/partial/`. The same cache class also accepts `VerifiedOpaque` XMPP wire records, which Iris currently constructs itself in its existing `media-ranges/` directory. | Make `LocalMediaStore` the sole factory/namespace and lifecycle authority for **both** partial-cache representations; move scheduling and network requests **out** of the storage layer |
| `NextcloudStorage` + `NextcloudWorker` | Pinned-ETag WebDAV reads; playback Range resolver; bounded encrypted cache | Uses storage-wide FIFO after PR #155 to avoid reentrancy of `collectReply()`; no independent-chunk parallelism or request cancellation |
| `IrisMediaSync` | One XMPP media sync per source, verified opaque chunks, gap/seek priority, and overlapping old/new Jingle content during BUNDLE handover (old partial chunk drains while new content is negotiated); different media syncs can coexist | Global admission/bandwidth budgeting across independent media and providers; retain existing safe Jingle overlap and receipt ordering |
| `MediaStream` / `MediaSource` | Seekable synchronous access to *already available* verified local bytes | Remain local-only; do not put network/blocking reads into `QIODevice::readData()` |

The common HTTP bridge already stops writing to a disconnected client, but a
plugin's in-flight fetch can continue. The existing first-GET-read priority rule
protects against *old* HTTP GET continuations overriding a newer seek; retain
that behavior when moving prioritization into a common coordinator.

## Target layering

```text
QMediaPlayer / media preview / future readers
                  |
                  v
     Player-range adapter (MediaRangeService)
     - HTTP GET/HEAD/Range, connection lifetime, socket backpressure
     - per-consumer cancellation on disconnect / player seek
                  |
                  v
     Verified-range coordinator
     - exact plaintext offset/length -> chunk indices + subranges
     - attach/detach consumers waiting on each verified chunk
     - satisfy local hits and emit available verified slices
     - reprioritize needed chunks; publish validated progress
            |                          |
            v                          v
     LocalMediaStore            Shared chunk scheduler
     - openPartialCache()       - single-flight, priorities
     - cache identities         - fair bounded parallelism
     - local key / encryption   - cancellation, prefetch
     - verified cache records   - network statistics
            ^                          |
            |                   +------+-------+
            |                   |              |
            |                 XMPP/Iris     Nextcloud/WebDAV
            |                   |              |
            +-- verified opaque-+              |
            +-- validated plaintext ---------+
            |
     reauthenticated requested slices
     returned to range coordinator
```

The diagram is logical. **The local partial cache is owned/provided by
`LocalMediaStore`, not by the transport plugin or the downloader.** The
coordinator receives a cache handle; source-specific validation is performed
before the shared store accepts a record. XMPP hands the store a verified,
already wire-encrypted AEAD record; Nextcloud hands it a validated plaintext
WebDAV chunk for local sealing under the profile key. Plugins retain only
account/source identification, transport and verification rules, not cache
directory construction, encryption keys or garbage collection. Do **not**
require Nextcloud to store AnyKeep ciphertext remotely; native Notes/WebDAV
clients must remain interoperable.

**Current deviation:** Iris still uses `MediaPartialChunkCache(VerifiedOpaque,
directory)` directly and its durable numeric files live in
`<AnyKeep data>/media-ranges/<hash>/`. The present
`LocalMediaStore::partialMediaCache()` supports only locally sealed
Nextcloud-shaped chunks, so the factory needs to be extended.

**Nightly-only cache compatibility policy:** no on-disk backwards compatibility
or cache migration is required for partial XMPP media. When switching to the
single `LocalMediaStore` cache namespace/format, obsolete `media-ranges/`
records may be safely discarded **as disposable remote-media cache**, then
reacquired from the authenticated remote source. An unreadable, stale, or
incompatible partial record is a cache miss, not a damaged note or permanent
sync failure. Cleanup must be limited to recognized obsolete *cache* paths;
never wipe media blobs, original local files, drafts, published media metadata
or note bodies. If offline, report unavailable/waiting and retry on reconnect
rather than marking missing bytes as verified. Do not weaken authenticated
chunk validation. This policy does **not** relax native Nextcloud Notes
interoperability or portable published note formats.

## Proposed contracts and invariants

### Resource identity and chunk states

- Source key identifies an immutable **account + remote object + revision +
  representation / chunk geometry**. Do not deduplicate across different
  credentials, notes, ETags, or incompatible encryption parameters. A trusted
  immutable content hash may deduplicate *verified content*, but never merge
  unverified transfers based on path or filename alone.
- Chunk state: `missing -> queued -> fetching -> verifying -> available` or
  `failed`. `available` means an authenticated record was atomically committed
  to the durable cache. Failure allows bounded retry; it never marks bytes
  verified. Disk revalidation after restart may demote a corrupted record.
- Each chunk has one in-flight transport fetch and a set of independent
  consumer waiters. Two requests for any part of a missing 1 MiB chunk attach
  to the same future; the second does not dispatch another fetch.
- A waiter identifies requested **plaintext** byte range, priority, owner,
  deadline/lifetime and cancellation token. Complete exactly once, including
  on shutdown, account switch, stale revision or provider destruction.
- After authentication, return the exact requested slices, not a whole file or
  plaintext disk copy. Bound buffers to the number of active chunk records and
  outstanding socket writes.

### Download scheduling

- Separate `required-now` decoder demand from speculative lookahead,
  user-requested offline sync and lower-priority background gap filling.
  A seek immediately reprioritizes demand; old readers can continue consuming
  verified data without preempting the new position.
- **Two independent controls:** admission limits (maximum simultaneous active
  remote media fetches) and a configurable aggregate *download bitrate* budget.
  Limiting fetch count alone does not prevent two fast transfers from saturating
  a user's connection. The global budget applies across XMPP, WebDAV and future
  plugins, not separately per storage instance or note.
- Begin with conservative app-wide and per-provider/per-host admission caps,
  a bounded queue, and per-source declared capabilities. Count actual active
  transport/network work, not logical waiters: several callers for the same
  chunk are still one transfer; overlapping Jingle content sharing a BUNDLE
  does not necessarily mean two independent network connections. Preserve
  existing Iris partial-chunk drain, `content-add` and receipt handover.
- Budget not only active request count but also total buffered/in-flight
  plaintext and ciphertext. Apply bounded backpressure before allocating
  another full 1 MiB record, particularly when many notes/videos are open.
- **Bandwidth control:** support an optional user-configurable aggregate cap
  (bytes/s) enforced at a common byte-accounting/admission point, e.g. a
  token-bucket or paced grants that can be reported by each transport. A strict
  aggregate *hard cap* requires transports with byte-level flow control; when
  Jingle/HTTP cannot pause safely, the scheduler can only limit admission and
  defer new work, not honestly promise precise instant-rate limiting. Never
  pause a Jingle stream halfway through an authenticated chunk merely to meet
  a rate target; preserve its existing safe drain/handover behavior.
- Reserve responsiveness for foreground seeks and small urgent media reads:
  allow them to displace speculative prefetch, but do not consume the entire
  network budget indefinitely. Use weighted fairness across independent media,
  storage accounts, and providers, and allow background work to yield under
  decoder underrun or network congestion.
- Add rolling measurements of *useful delivered bytes/sec*, time to first
  verified chunk, RTT/setup cost, error/retry rate, stalled playback and queue
  time. Adapt admission gradually (increase conservatively, reduce quickly
  after errors, latency spikes or congestion), with cooldown/hysteresis.
  Throughput alone cannot distinguish link saturation from a slow remote
  endpoint. Changing network, metered connections and user policy can demand
  lower caps.
- The scheduler can only account for traffic it manages; other application
  uploads, calls and unrelated processes can compete for bandwidth. Eventually
  expose a shared traffic budget to AnyKeep's uploads too. Never equate adaptive
  concurrency with a guarantee of leaving bandwidth free for the rest of the
  system.
- A canceled decoder GET removes that *consumer's* demand and reprioritizes
  remaining waiters. Cancel the underlying fetch only when no playback,
  prefetch, or durable sync demand remains **and** the transport can safely
  cancel it. Finishing an almost-complete authenticated record may be cheaper
  than aborting it; never expose unauthenticated partial contents.
- Avoid starvation: priorities are hints, not permanent exclusion. Protect
  responsiveness for an active seek while eventually serving other readers or
  independently requested offline synchronization.

### Integrity and earliest safe delivery

- The coordinator knows exact offsets and can release requested subranges when
  their **authentication units** are verified; the HTTP adapter need not wait
  for the full media object.
- The existing XMPP `MediaChunkWire` uses an AEAD tag per complete wire chunk.
  **It cannot safely stream the plaintext of half a chunk** before that tag
  validates. Likewise, a locally `SecureEnvelope`-sealed cache record must
  be opened and authenticated as a record before reading plaintext.
- Nextcloud native media is plaintext over TLS, guarded by the pinned strong
  WebDAV ETag, `If-Match`, validated `206`/`Content-Range`, and bounded
  read sizes. That is *not* AnyKeep end-to-end content authentication. Early
  in-flight forwarding would require an explicit per-source trust/error policy
  (including short reads and mid-transfer failures). Initially deliver only
  fully validated chunks; 1 MiB chunks already provide bounded startup.
- Future streaming sub-chunk delivery under E2EE requires smaller independently
  authenticated records or a different cryptographic construction, with manifest
  format and interoperability implications. Do not pretend partial AEAD
  decryption gives authenticated plaintext.

### Threading, lifecycle and observability

- All scheduler state belongs to one chosen dispatcher thread; providers
  implement asynchronous fetches and return immutable results. No nested
  `QEventLoop::exec()` in the downloader network path.
- Never call a plugin's mutable worker configuration while another operation
  can reenter it. Pass an immutable account/source snapshot to each fetch.
- Transport cancellation, storage shutdown, player disconnect and revision
  changes must each settle outstanding waiters without hanging HTTP sockets.
- `MediaSyncService` continues to report only durably authenticated plaintext
  bytes, not bytes received over the wire or already handed to a decoder.
  Throughput/latency and decoder buffer health are separate transient metrics.

## Suggested implementation sequence

1. **Behavioral tests and boundaries.** Add tests for overlapping/multiple
   chunk ranges, single-flight, disconnected clients, rapid seek, provider
   cancellation, revised ETag, changed account, corruption, restart, error
   propagation and deterministic concurrency ceilings. Retain XMPP Jingle
   handover/receipt regression coverage.
2. **Unify partial-cache ownership and extract the coordinator.** Extend
   `LocalMediaStore` with a handle/factory for locally sealed plaintext
   records and verified opaque XMPP wire records. Use a single cache namespace
   and current format, with no migration/compatibility reader for the obsolete
   XMPP `media-ranges/` partial cache: invalidate it, treat it as a miss and
   resynchronize remotely. Do not delete original managed blobs or notes.
   Centralize cache identity, quotas and eventual reference-aware eviction
   in the shared store, **not** in transport plugins. Move waiters, chunk
   state, geometry and priority handling out of `NextcloudStorage`
   into a separate shared coordinator. Keep the actual download scheduler
   separate from the store. Initially use concurrency = 1 for Nextcloud to
   preserve PR #155 behavior. Keep the existing XMPP transfer implementation
   until its lifecycle fits the coordinator contract.
3. **Asynchronous WebDAV source.** Remove synchronous `collectReply()` from
   the remote-media path, implement abortable `QNetworkReply` fetches with
   immutable per-request credentials and ETag validation; enable a tested small
   parallelism cap for *different* Nextcloud chunks.
4. **Budget shared network traffic, preserve Iris parallelism.** Admit Jingle
   and WebDAV work through the same global transfer budget while retaining
   existing Iris overlapping BUNDLE content and its drain/receipt semantics.
   Let the coordinator choose high-priority missing chunks, coalesce waiters,
   and count physical transport work rather than Jingle content count.
5. **Adaptive scheduling and prefetch.** Add per-network/provider metrics,
   user-configurable bandwidth/parallelism policies and conservative lookahead.
   Use a real pacing mechanism only where byte-level flow control is safe.
   Bound memory, sockets and disk growth; add cache eviction/orphan collection
   only as a separate, reference-aware lifecycle design.

## Acceptance criteria for first code PR

- All media plugins obtain partial cache handles from `LocalMediaStore`,
  with isolated immutable source identities and no plugin-specific cache
  directory or profile-key handling. Obsolete XMPP partial cache records
  are safely treated as cache misses and re-fetched; no legacy path reader
  or format migration is required.
- Two simultaneous decoder reads of one uncached chunk cause exactly one
  transport request and return correct byte slices to both clients.
- Requests for different chunks can be scheduled independently without
  re-entering provider code; total concurrent *active transfers* and buffer
  allocations never exceed global/per-source budgets and transport capability.
- Independently active XMPP and WebDAV sources share the admission budget,
  while an existing Jingle BUNDLE handover may overlap safely and complete
  deferred receipts without deadlock or regression.
- Foreground playback keeps priority and background prefetch yields under
  congestion. An explicit throughput cap is tested only for transports that
  expose reliable byte-level pacing; all others document best-effort admission
  rather than claiming a bandwidth guarantee.
- Aborting one client request never aborts another client's shared chunk;
  with no consumers, cancellation honors active durable sync demand.
- A seek takes priority over prefetch and stale GET continuations.
- No bytes from an incorrect account/revision, unverified record, or partially
  authenticated AEAD chunk are exposed; progress counts only verified durable
  bytes.
- Shutdown and source switching settle every waiter promptly; no UI blocking.
- XMPP authenticated *remote wire protocol*, native Nextcloud Markdown/files,
  portable note/attachment data, and existing cross-platform media behavior
  remain compatible; **local XMPP partial-cache disk format is explicitly
  exempt** from backward compatibility.
