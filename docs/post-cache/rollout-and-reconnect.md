# Post cache: rollout and reconnect validation

> Part of [Post cache architecture](../post-cache.md). Read the landing page first and load this file only when this area is relevant.

## Implementation phases

### Phase 1 — durable store

Implemented in this PR:

- `PostCacheStore` and SQLite schema;
- compressed raw JSON;
- account isolation;
- LRU limits and timed incremental vacuum;
- unit tests for round-trip, isolation, channel/thread selection and eviction.

### Phase 2 — service integration and admission

Implemented/in progress in this PR:

- dedicated asynchronous `PostCacheService` / SQLite worker thread;
- capture account identity with every queued operation;
- causal observation fencing for stale HTTP versus newer WebSocket invalidations;
- write successful HTTP post objects through to SQLite;
- WebSocket new/edit write-through and delete/reaction invalidation;
- channel-open admission policy: one hour for memory, ten hours for disk;
- precise root-lease resident admission for replies to an actually open thread even after its parent
  channel becomes memory-cold;
- seed channel-open time from Mattermost preferences and refresh it on local activation;
- dedicated Cache settings tab for all user-facing limits.

Implemented prerequisites for reads:

- full in-place refresh semantics when fresh JSON arrives for an already resident `BackendPost`;
- resident causal fencing against stale HTTP responses.

Read-side status:

- direct `loadPost()` cache hit followed by background HTTP validation is implemented;
- provenance-backed newest channel/thread window hydration is implemented as provisional first paint
  followed immediately by normal authoritative HTTP validation;
- arbitrary cached row bags are never packed into a logical range.

### Phase 3 — bounded resident cache

- replace durable raw-pointer assumptions with explicit resident leases/ID resolution;
- keep transient WebSocket processing for cold channels while admitting only the narrow open-thread
  root/reply case required by an active lease;
- stop reconnect post-page materialization for channels outside the memory horizon;
- add per-post memory-cost accounting;
- 30-second sweeper, 5-minute cold TTL, one-hour channel horizon, 500 MiB hard / ~400 MiB target;
- remove cold `BackendPost` objects and compact auxiliary containers;
- rematerialize on source demand from SQLite, then HTTP.

### Phase 4 — restart validation and tuning

- restore eligible cached newest suffixes immediately on channel/thread open;
- validate in the background without moving the viewport;
- expose cache statistics/logging for tuning limits and vacuum thresholds.

## Timeline authority and reconnect validation

Persistent caching must not make Mattermost's approximate message counts an
authority for post identity. `total_msg_count_root` is useful for scrollbar scale and for choosing an
initial disconnected seek position, but it is not `/posts` row count: deleted roots can make it too
large, while join/leave and other count-excluded system roots can make it too small. Concurrent server
changes can add another source of disagreement.

The channel source keeps separate transport paths with explicit authority:

1. demand touching the known newest edge uses one `page=0` request sized for that contiguous demand;
2. ordinary sequential scrolling from an authoritative identity uses `before=<post_id>` /
   `after=<post_id>` cursor requests;
3. genuinely disconnected random positioning may use absolute `/posts?page=N&per_page=10` pages;
4. oldest-boundary repair uses absolute-page probes because that is the evidence needed to correct the
   approximate `total_msg_count_root` coordinate space.

The ten-post page size remains a useful minimum/fallback for cursor and absolute-page work, but it is
not a `LongListWidget` request-block invariant. The view may ask for a larger contiguous range (for
example a full viewport plus prefetch); the source chooses one efficient edge/cursor request rather
than turning every ten logical indices into a separate HTTP job.

Absolute pages remain newest-anchored for disconnected placement and boundary repair. A successful
empty absolute page is authoritative boundary evidence. A large top-edge request starts with a
one-root probe 3% inside the estimate. Empty results search inward; existing data searches outward to
the estimate and, when the reported oldest page is full, continues beyond it until `/posts` proves the
real edge. Small estimates use a normal ten-post page first and expand outward if that page disproves
the count. Exact reconciliation removes or inserts an oldest logical prefix while preserving already
known newest identities. The 3% value is a latency heuristic, never a correctness assumption.

Every successful range request ends in one of three states: new identities were placed, a real
boundary removed stale logical slots, or the request made no progress and is finished without an
immediate retry loop. Overlapping/adjacent logical demand may attach to a compatible exact in-flight
source request, but that attachment does not grow into a FIFO prefetch chain; a distant fast-scroll
request remains free to start independently.

WebSocket reconnect uses the same bounded-working-set rule. A successful Mattermost sequence resume
requires no HTTP history replay. If reliable replay explicitly fails, only the currently viewed
conversation is validated immediately; inactive joined channels are left lazy and are validated when
opened. This prevents a reconnect from filling either the network queue or the post cache with
channels the user is not reading.

Cache admission follows user interest rather than membership. By default a channel is eligible for
resident-memory post caching for one hour after it was viewed, and for persistent SQLite post caching
for ten hours after it was viewed. The current channel is always considered interested. An open thread
may keep only its leased root and newly arriving replies for that root resident after the parent
channel ages cold; this does not refresh the channel's interest horizon. These intervals, memory/disk
limits, maintenance cadence, TTLs and vacuum controls are exposed on the cache settings page and are
policy inputs rather than timeline geometry.
