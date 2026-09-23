# Post cache runtime: policy and tests

> Part of [Post cache runtime contract](../post-cache-runtime.md). Read the landing page first and load this file only when this area is relevant.

## Persistent store policy

The durable cache is bounded and disposable. Current defaults:

- one SQLite database under `QStandardPaths::CacheLocation`;
- account-scoped rows;
- channels opened within 10 hours eligible for retention;
- at most 10,000 posts globally;
- at most 1,000 replies per thread;
- at most 5 GiB compressed payload budget;
- write-time limit enforcement with LRU trimming;
- periodic maintenance even without reads.

SQLite invariants:

```text
page_size = 4096
journal_mode = WAL
synchronous = NORMAL
auto_vacuum = INCREMENTAL
```

Maintenance removes expired-channel rows, reapplies limits, optimizes indexes, checkpoints/truncates
WAL and performs bounded incremental vacuum when free-page thresholds are met. Full `VACUUM` is
avoided because a multi-gigabyte cache should not require another database-sized temporary file.

## Resident-memory runtime

The resident-memory limiter uses:

- channel-open memory admission horizon: 1 hour;
- 500 MiB hard accounted limit;
- pressure trim target around 400 MiB;
- 5-minute idle TTL for unleased cold posts;
- 30-second independent sweep;
- explicit leases for visible widgets, active edit/reply contexts and active thread roots;
- source IDs surviving body eviction so they can be rematerialized from SQLite/HTTP.

`PostResidencyLease` is the move-only RAII pin for retained raw-reference/semantic-body dependencies.
Materialized `PostWidget`s, active edit/reply composer contexts and active thread roots hold leases.
Pinned-dialog copies do not accidentally pin a same-ID resident body because acquisition verifies
object ownership in `BackendChannel::postIdToPost`.

The legacy `BackendPost::rootPost` relationship is still handled conservatively: a root is not
eligible for eviction while a resident reply names that root. The durable relationship remains
`root_id`.

The 30-second sweeper removes eligible unleased bodies after idle TTL and enforces the accounted memory
limit. Accounted bytes are a stable approximation based on compact raw JSON/dynamic data, not process
RSS. Leases/dependency pins are never violated even if that temporarily leaves the cache above target.

The open-thread WebSocket exception above is an admission rule, not a replacement for leases: the root
lease proves that this particular thread remains active; newly delivered replies then participate in
normal residency/lease/TTL policy.

## Failure behavior

Cache failure must degrade to ordinary network behavior:

- SQLite open/read failure -> cache miss, continue HTTP;
- corrupt/undecodable row -> miss; authoritative HTTP may replace it;
- cache write failure -> lose warming only, never user/server state;
- stale HTTP rejected by resident fence -> other posts in the same response may remain useful;
- stale queued disk write rejected by invalidation fence -> row remains absent/newer;
- channel no longer generally resident when HTTP succeeds -> disk may still warm if captured
  account/channel admission permits it;
- no cache result may move the scrollbar or declare a server boundary by itself.

The cache is always optional correctness-wise.

## Write/invalidation matrix

| Event/source | Resident action | Disk action |
| --- | --- | --- |
| HTTP full post/page/thread snapshot | causally accepted full in-place ingest | queue upsert if disk-eligible |
| WebSocket `posted` full snapshot | add/update if channel memory-eligible **or reply targets a leased open-thread root**; otherwise transient; always advance fence | queue upsert only if disk-eligible |
| WebSocket `post_edited` full snapshot | in-place refresh if resident; advance fence | queue upsert if disk-eligible |
| WebSocket delete | tombstone/remove according to UI semantics; advance fence | unconditional remove/invalidate |
| WebSocket reaction add/remove | mutate resident reaction if resident; advance fence | unconditional invalidate because event is not a full snapshot |
| SQLite direct read | insert only if absent and unfenced | none |

## What the cache never owns

The cache layer must not:

- calculate scrollbar pixels;
- decide viewport anchoring;
- create logical gap widgets;
- choose channel page/cursor authority from cached timestamps;
- choose thread adjacency solely from cached timestamps;
- refresh a newer resident object from SQLite;
- treat message traffic as channel-interest renewal;
- turn the open-thread reply exception into whole-channel admission;
- mix queued operations across accounts;
- block UI/network callbacks on SQL commits.

## Implementation status

Implemented in the current architecture:

- account-scoped `PostCacheStore` schema and compressed raw JSON;
- write-time limits, LRU, WAL maintenance and incremental vacuum;
- dedicated asynchronous `PostCacheService` worker thread;
- account identity captured per queued operation;
- HTTP write-through for post-bearing responses;
- WebSocket new/edit write-through plus delete/reaction invalidation;
- disk invalidation watermarks;
- full stable-address `BackendPost` refresh;
- resident observation fencing against stale HTTP;
- direct cache-first `loadPost()` with mandatory HTTP validation;
- provenance-backed newest channel/thread window hydration as provisional first paint;
- resident leases and bounded resident-memory policy;
- precise resident admission of live replies whose root is leased by an open thread;
- source-side demand planning/attachment separated from repository physical request coalescing.

Further work should preserve those contracts rather than reintroduce fixed-block view paging or
whole-channel admission as shortcuts.

## Required tests for future changes

Any cache/source change that affects read authority or causality should cover at least these races:

1. cache hit arrives before HTTP and caller completes once;
2. HTTP wins before cache read; late cache cannot overwrite resident data;
3. WebSocket edit/delete arrives after HTTP dispatch but before HTTP response; stale HTTP is rejected;
4. queued stale disk write follows a newer invalidation; row is not resurrected;
5. account switches while old operations remain queued; rows stay in original account namespace;
6. cached newest suffix paints without claiming server edge/cursor authority;
7. authoritative page/cursor overlap replaces provisional identities without duplicate IDs;
8. repeated identical authoritative window is a source no-op and cannot trigger a request loop;
9. reply to a leased open-thread root remains resident/delivered after the parent channel becomes
   memory-cold while unrelated cold-channel posts stay transient;
10. source request attachment accepts overlap/immediate adjacency, does not expand expected coverage,
    and allows distant fast-scroll demand to proceed independently.
