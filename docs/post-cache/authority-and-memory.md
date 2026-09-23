# Post cache: authority and resident memory

> Part of [Post cache architecture](../post-cache.md). Read the landing page first and load this file only when this area is relevant.

## Cache authority

A cached post identity/payload is useful data, but cached timestamps are **not** sufficient evidence
for an absolute Mattermost page number or cursor adjacency. New posts can shift absolute page
boundaries, and an arbitrary bag of cached IDs does not prove a contiguous server window.

Therefore:

- direct post lookup may eventually be satisfied from SQLite immediately;
- a bounded newest suffix may eventually seed a channel/thread resident model;
- HTTP remains authoritative for channel/thread edge placement, cursor adjacency and for proving
  oldest/newest boundaries;
- stale successful work may populate SQLite only when the channel remains cache-eligible, but it
  never gains viewport authority by itself.

This preserves the provisional/authoritative rules in `long-list-architecture.md`.

Cache-first **window** hydration is deliberately not enabled yet, but the resident refresh prerequisite
and direct-post read path are now in place. `BackendChannel::mergePostContext()` refreshes an
already-resident ID in place from the accepted full JSON snapshot, preserving the stable
`BackendPost*` address used by current widgets and sources. `BackendPost` replaces all server-backed
post fields rather than only the message, while preserving separately fetched poll metadata and local
annotations absent from raw REST/cache JSON.

Direct `loadPost()` is cache-first: an asynchronous SQLite hit may insert an absent resident post and
satisfy the caller immediately, but it never refreshes an already-resident object and never advances
the resident server-observation watermark. The normal HTTP request is still dispatched and
validates/refreshes that object in the background; a failed result is delivered only after both cache
and HTTP miss. Cached identity/timestamps still have no absolute-page authority. Newest channel and
thread window hydration remains the next read-side step and must stay provisional until endpoint-specific
HTTP boundary evidence arrives.

## Write-through and invalidation

The write side of service integration is active.

Successful post-bearing REST responses handled by `PostRepository` queue full `posts` objects into
SQLite before resident ingestion only for channels admitted by the disk-interest policy. This covers
direct post retrieval, channel pages, channel cursor windows and thread windows.

WebSocket handling follows the same durable rules:

- new post: upsert the event's full post object only if its channel is disk-eligible;
- edited post: upsert the event's full post object only if its channel is disk-eligible;
- deleted post: remove the cached row regardless of eligibility;
- reaction added/removed: invalidate the cached row regardless of eligibility because these events
  do not contain a lossless full replacement post object.

The open-thread root-lease exception affects **resident delivery only**. It does not bypass the disk
admission rule: a live reply delivered to an open thread in a disk-cold channel is not persisted merely
because the root is leased.

Deleting/invalidation is preferable to keeping a known-stale reaction/deletion snapshot. A later
REST fetch can repopulate that row only if the channel is still eligible.

## Resident-memory policy

The persistent cache makes a large resident history unnecessary. The target resident policy is:

- only channels opened within **1 hour** are generally eligible for resident post bodies;
- **500 MiB hard accounted maximum** across all materialized post models;
- trim back to roughly **400 MiB** after crossing the hard limit to avoid immediate churn;
- cold-post idle TTL: **5 minutes** by default inside still-eligible channels;
- active sweep every **30 seconds**, independent of cache lookups;
- LRU is a secondary pressure rule after channel eligibility and TTL;
- posts currently pinned by a materialized widget, active edit/reply context, active thread root or
  another explicit lease are not evicted until their lease is released.

The one-hour horizon is based on channel-open time, not last message activity. WebSocket traffic for
a cold channel may still update unread/mention/notification metadata, but the event's full post body
must not become a durable `BackendPost` merely because the server delivered it. The narrow exception
is a reply to the root leased by an actually open thread: that reply is admitted so the visible thread
receives its live body, without making the parent channel hot or admitting unrelated roots/replies.
Reconnect recovery must likewise avoid fetching/materializing post pages for every joined channel.

The 500 MiB value is cache-accounted memory, not process RSS: portable C++ cannot reliably attribute
allocator arenas and Qt internals to one cache. Each resident post will carry an estimated retained
cost based on its compact JSON size plus materialized dynamic data. The estimate should be biased
upward so the cache trims before real heap usage becomes problematic.

A resident sweep must run from its own timer. It must not depend on a future lookup to discover that
old entries expired.

### Memory compaction

Resident eviction must actually erase the owning `BackendPost` object and its identity-map entry;
clearing only widgets is not a memory cache eviction. After bulk eviction, auxiliary vectors/maps
that retain excess capacity should be rebuilt or `squeeze()`d where applicable. `std::list` nodes
are released immediately; global allocator RSS returning to the OS remains allocator/platform
specific and is not a correctness invariant.

Before true resident eviction is enabled, raw pointer lifetime has to be made explicit. In
particular:

- `BackendPost::rootPost` cannot be a durable owning/reference mechanism; `root_id` is authoritative;
- a visible `PostWidget` must hold a lease preventing its backing post from disappearing;
- an open `ThreadPostSource` holds a lease on its root so its live reply admission remains valid;
- sources keep post IDs, not permanent raw pointers;
- an evicted ID must be rematerializable from SQLite or HTTP without changing its logical identity.

This pointer/lease step is required before enforcing the 500 MiB resident cap. Adding an unsafe
`erase()` to the current `BackendChannel::posts` list would introduce use-after-free bugs.

## Settings surface

Cache policy is user-configurable from a dedicated **Cache** tab rather than being mixed into the
attachment/download form. The user-facing knobs are:

### Attachment files

- maximum attachment-file disk cache size.

### Post cache on disk

- channel-open horizon, default 10 hours;
- maximum compressed payload, default 5 GiB;
- maximum total posts, default 10,000;
- maximum replies per thread, default 1,000;
- maintenance interval, default 10 minutes.

### Post cache in memory

- channel-open horizon, default 60 minutes;
- hard accounted limit, default 500 MiB;
- trim target after pressure, default 400 MiB;
- cold-post TTL, default 5 minutes;
- sweep interval, default 30 seconds.

SQLite page size, journal mode, vacuum free-page threshold and bounded vacuum chunk size remain
implementation invariants. Exposing them as normal user settings would make it easy to create cache
layouts that violate the storage assumptions without providing meaningful product-level control.
