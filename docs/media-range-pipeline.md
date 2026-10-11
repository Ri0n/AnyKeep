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
parallelism, but allow each source to declare its constraints (e.g. WebDAV permits
concurrent HTTP ranges; the existing Iris/Jingle BUNDLE state machine has its
own range handover and may need one active receive operation).

## Current implementation (October 2026)

| Component | Existing responsibility | Missing for the target design |
| --- | --- | --- |
| `MediaRangeService` | Loopback HTTP server for `QMediaPlayer`: parses HTTP Range; asynchronously requests 64 KiB slices; throttles socket writes; only the first read of each GET may reprioritize retrieval | Cancellation propagated upstream on socket disconnect, explicit per-player demand/session tracking |
| `MediaSyncService` | Publishes and observes *durably verified* byte counts and download states | Intentionally not a scheduler; keep it separate |
| `MediaPartialChunkCache` | Provider-neutral atomic persistent cache; `VerifiedOpaque` for XMPP wire chunks and locally `SecureEnvelope`-sealed chunks for Nextcloud; reads reauthenticate | Centralized in-flight/scheduled state and coalescing (must not be added to the persistence class) |
| `NextcloudStorage` + `NextcloudWorker` | Pinned-ETag WebDAV reads; playback Range resolver; bounded encrypted cache | Uses storage-wide FIFO after PR #155 to avoid reentrancy of `collectReply()`; no independent-chunk parallelism or request cancellation |
| `IrisMediaSync` | One XMPP media synchronization session, verified opaque chunks, next-gap selection, seek priority, reconnect/recovery | Provider-wide scheduling independent of the Iris-specific session state |
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
                  |
                  v
     Shared chunk scheduler / downloader
     - single-flight per immutable object and chunk index
     - demand priority, fair scheduling, bounded parallelism
     - cancellation and optional background/prefetch demand
     - latency/throughput/backpressure measurements
                  |
           +------+-------+
           |              |
         XMPP/Iris       Nextcloud/WebDAV        (future sources)
         chunk source    HTTP chunk source
           |              |
           +------+-------+
                  |
        per-source authentication / validation
                  |
        MediaPartialChunkCache
        durable verified records
                  |
        verified plaintext to coordinator
```

The diagram is logical; a source may hand an authenticated opaque wire record
to its verifier, or a validated plaintext response to the locally sealed cache.
Do **not** require Nextcloud to store AnyKeep ciphertext remotely; native Notes
and WebDAV clients must remain interoperable.

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
- Start with deterministic caps, not automatic unlimited parallelism: a small
  global cap plus per-provider/per-host concurrency ceilings, declared source
  capabilities and a bounded queue. Provider-specific transport rules are
  authoritative; an Iris Jingle BUNDLE cannot be made parallel by a generic HTTP
  connection limit.
- Only after instrumentation, consider adaptive concurrency using rolling
  throughput, transfer setup latency, RTT/failure rate, network changes and
  decoder buffer health. Throughput alone is insufficient to distinguish a
  saturated link from a slow remote endpoint.
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
2. **Extract common coordinator.** Keep existing transport adapters and cache
   record formats. Move waiters, chunk state, geometry and priority handling out
   of `NextcloudStorage` into shared code. Initially use concurrency = 1 for
   Nextcloud to preserve PR #155 behavior. Keep the existing XMPP transport as
   source-specific ownership until its lifecycle fits the new contract.
3. **Asynchronous WebDAV source.** Remove synchronous `collectReply()` from
   the remote-media path, implement abortable `QNetworkReply` fetches with
   immutable per-request credentials and ETag validation; enable a tested small
   parallelism cap for *different* Nextcloud chunks.
4. **Move Iris gap scheduling behind source capabilities.** Keep the current
   Jingle BUNDLE/receipt safety rules while allowing the coordinator to choose
   high-priority missing chunks and coalesce playback waiters.
5. **Adaptive scheduling and prefetch.** Add per-network/provider metrics and
   conservative optional lookahead. Bound memory, sockets and disk growth;
   add cache eviction/orphan collection only as a separate, reference-aware
   lifecycle design.

## Acceptance criteria for first code PR

- Two simultaneous decoder reads of one uncached chunk cause exactly one
  transport request and return correct byte slices to both clients.
- Requests for different chunks can be scheduled independently without
  re-entering provider code; concurrent fetch count never exceeds policy
  and provider capability.
- Aborting one client request never aborts another client's shared chunk;
  with no consumers, cancellation honors active durable sync demand.
- A seek takes priority over prefetch and stale GET continuations.
- No bytes from an incorrect account/revision, unverified record, or partially
  authenticated AEAD chunk are exposed; progress counts only verified durable
  bytes.
- Shutdown and source switching settle every waiter promptly; no UI blocking.
- XMPP existing wire/cache format, Nextcloud-native Markdown/files, and
  existing cross-platform media tests remain compatible.
