# Thread timeline loading contract

This document defines the data-loading invariants for partially available chat and thread timelines.
It complements `long-list-architecture.md`; the ownership rules there remain authoritative. Shared
logical ID-slot bookkeeping and source-side request attachment are documented in
`post-source-architecture.md`; this document covers only thread-specific topology and transport
authority.

## Prefetch before a logical gap

`LongListWidget` owns the decision that more logical items are needed. A post source must not inspect
scrollbar pixels or viewport geometry to implement prefetch.

For ordinary scrolling, reserve half the estimated screen capacity on each side of the visible
range. Screen capacity is viewport height divided by the mean full height of its visible rows.
Measured row heights are used when available; otherwise the existing height estimates apply.
The logical margin is rounded up, with a minimum of one neighbour for very tall messages.
For example, ten messages per screen produce a five-message margin; six produce three.
Normal geometry synchronization recalculates this margin after row reflow or viewport resizing.

Post lists have no additional full-screen buffer. Other consumers may explicitly configure an
extra screen/pixel buffer; their desired range includes the union of that buffer and the adaptive
logical margin. HTTP page sizes remain independent of this viewport demand.

For ordinary viewport work `LongListWidget` reports the contiguous missing logical demand instead of
splitting it into transport-sized ten-item blocks. If the desired tail is `155..168`, the source sees
that whole range and can satisfy it with one edge request. Transport minimums/page sizes remain a
source/repository concern.

Random seek is different: a genuinely disconnected thumb jump still uses a small seed window around
the target so a huge estimated viewport range does not become one giant speculative HTTP request.

## Thread logical-index authority

An exact-count thread uses oldest-to-newest logical indices:

```text
0                  root post
1..reply_count     replies, oldest -> newest
```

Only data with known positional provenance may claim an exact logical index.

Authoritative placements are:

- the initial page at the oldest boundary;
- the tail page at the newest boundary;
- a page fetched immediately before or after an already mapped post cursor.

A partial `BackendChannel` cache is **not** automatically an authoritative contiguous page. It may
contain a tail page, an older context window, or several disjoint windows. Unless all replies are
known, the source must not pack arbitrary cached replies into a synthetic prefix or suffix merely to
fill logical slots.

## Cursor demand convergence

`ThreadPostSource` owns one demand across as many transport pages as necessary. It selects the
first missing run and the nearest authoritative cursor outside that run, or the newest boundary.
The cursor need not be numerically adjacent to the request. Each response advances a proven
frontier by the number of returned replies, in batches of at most 50:

```text
known suffix       [436..731]
requested          [382..391]
cursor-before      [423..435]
cursor-before      [410..422]
cursor-before      [397..409]
cursor-before      [384..396]
cursor-before      [374..383]
finish             all requested rows available
```

Newest-tail retrieval uses `direction=up` without `fromCreateAt` or `fromPost`. A root snapshot
may omit `last_reply_at`, or carry an old value. Using `last_reply_at + 1` as the tail cursor
then returns either no replies or an old page, neither of which proves the newest boundary.

`has_next=false` (including an omitted field normalized to false) is not a standalone count proof.
An empty page must not shrink 732 logical items to just the root. Count updates come from root
summary ingestion; empty/inconsistent pages fail demand without erasing existing mappings.

Short transport pages do not finish demand. Multiple missing runs are filled in the same demand.
Ordinary demands touching an active page's expected range attach to the source's existing demand.
Their original ranges are retained for completion notifications, and their combined interest is
filled by subsequent exact pages if necessary. Attaching does not enlarge the in-flight page's
expected range. This avoids overlapping requests with different `perPage` URLs; repository HTTP
coalescing still handles identical URLs. New seeks never attach behind old scrolling work. A newer
seek generation stops older cursor chains after their current response, completing every attached
request; ordinary scrolling with generation zero remains valid.

The maximum exact page size remains 50. Nearby scrollbar seeks (within 30 rows of a proven
cursor or boundary) use this path. Distant scrollbar seeks use a bounded timestamp island instead.
Network tests bound physical requests, returned replies and serialized response bytes, and verify
that overlapping, adjacent and contained ordinary demands share an in-flight page.

Mattermost uses an exclusive `(fromCreateAt, fromPost)` cursor. The repository strips the root
included in every response and normalizes replies to oldest-to-newest order. Cursor timestamps live
in source metadata, independently of evictable `BackendPost` bodies. Requested bodies have residency
leases across the constituent pages; off-target bodies may be evicted immediately.

### Permalink islands and numeric seek

A timestamp does not prove an ordinal position. A distant scrollbar seek estimates time between
the nearest proven index/time anchors, using root/latest-reply metadata outside known windows.
It fetches one page of at most 30 replies and presents it as a provisional island near the requested
index. Nonuniform activity can make the actual rank very different from that estimate. The scrollbar
still uses logical item indices and estimated/measured row heights, not elapsed time.

If the initial page already overlaps authoritative IDs or proves the newest boundary, the source
reports the resolved target for the active seek generation. This lets the viewport follow the
returned context even when its proven position is far from the requested estimate. Stale responses
may warm the body cache but cannot move the viewport or place an obsolete seek island.

A permalink instead names a semantic identity. It loads up to 15 replies before and 15 after that
identity in two requests and displays this connected island immediately. It does not enumerate the
intervening replies from an authoritative boundary. The target's timestamp may supply an initial
scrollbar-position estimate; that estimate never establishes an authoritative rank.

An island retains its ordered IDs and lightweight cursor metadata independently of resident bodies.
Scrolling beyond its edges fetches small cursor pages (10–30 replies); evicted bodies can be fetched
again using the same metadata. Overlapping island demands share the in-flight context load.
Navigation and highlighting require available context, not a globally proven ordinal.

Multiple islands retain their mappings between seeks. Shared identities establish their relative
origins and allow their connected windows to merge. Provisional islands have an empty guard slot
on each side. If an exact page would numerically touch
an island without shared identities, the island relocates instead of claiming semantic adjacency.
Identity overlap or an explicitly reported source boundary can prove its rank and promote its
mapping. The view carries existing widgets and measured heights by identity, adjusting its scrollbar
so the visible message keeps its screen position. An omitted pagination flag is not boundary evidence. Visible post identity remains the
navigation anchor when placement changes.

### Completion and errors

A cursor demand finishes when its requested rows are available, a proven count correction removes the
requested rows, or a newer seek supersedes it. Transport failures, empty no-progress responses and
conflicts with confirmed ranks emit `rangeRequestFailed` and a warning before completion. A new user
request may retry; the source does not silently repeat the same cursor forever. An approximate seek
finishes after its renderable context is placed and its resolved position is reported. Its original
numeric range is an estimate, not a requirement to traverse the intervening history.

`LongListWidget` reports demand and does not poll unchanged missing ranges. When measuring newly
materialized rows changes the desired logical viewport, it schedules another geometry sync. This
ensures shorter real rows do not leave uncovered viewport edges after the initial seek block fills.

## Regression coverage

`thread-post-source-integration-test` links the production source, repository, HTTP connector and
LongList implementation. Its loopback HTTP fixture uses 732 logical items, nonuniform timestamps,
exclusive compound cursors, root inclusion and short pages. It checks exact IDs, completion timing,
off-target body eviction, all four reported seek geometries, overlapping generations, permalink
relocation, bounded permalink and scrollbar loading in a 10,000-reply thread, island expansion and body rehydration,
identity-based merging/promotion with stable widgets and viewport Y, explicit failure/retry, count correction, missing/stale summary timestamps, empty boundary
responses, omitted pagination flags, and actual widgets after random scrollbar moves.
The HTTP pagination fixture follows Mattermost's
[thread store implementation](https://github.com/mattermost/mattermost/blob/master/server/channels/store/sqlstore/post_store.go).

Run with:

```sh
cmake --build build --target thread-post-source-integration-test
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R thread-post-source-integration-test --output-on-failure
```

This deterministic regression suite does not replace runtime verification on the original long
thread. Enable `mattermost.timeline.thread.debug=true` and `mattermost.timeline.trace.debug=true`
for that verification.

## Count and boundary differences from channel history

Channel history needs an absolute-page boundary repair because `total_msg_count_root` is not the
row count of ordinary `/channels/{id}/posts`: deleted roots can make it too large, while count-excluded
system roots can make it too small. Threads deliberately do not inherit that page-probing algorithm.
Mattermost `Thread.ReplyCount` excludes deleted replies, and the thread endpoint
is cursor/time based (`fromCreateAt`, `fromPost`, `direction`) rather than an absolute `page=N` space.

If a future server/plugin configuration produces a real thread count mismatch, only the structural
slot mutation belongs in the shared `IndexedPostSource` layer. The proof that a particular count is
correct remains thread-specific, just as `/posts` page-boundary proof remains channel-specific.
`LongListWidget` must stay unaware of Mattermost counts, pages and cursors.

## Live replies in an open thread

An open `ThreadPostSource` holds a residency lease on its root post. That lease also acts as the
precise WebSocket admission signal for replies to that root: even if the parent channel is outside the
normal resident-channel horizon, a `posted` reply for the leased root must be inserted into
`BackendChannel` and delivered through `channel.onNewPost`.

This avoids the old failure mode where the server delivered the reply body over WebSocket, but the
client classified the parent channel as cold, emitted only the global transient notification, and the
open thread never saw the reply. The exception is deliberately root-specific; it must not make the
entire parent channel resident.

## Stability requirements

A page response that does not change logical identity mapping must not cause existing visible
`PostWidget`s to be destroyed and recreated.

Newly filled empty slots require availability notification. `itemsChanged` is reserved for logical
indices whose previously concrete identity/content really changed.

Changes to a root post's collapsed-thread metadata, such as reply count or participant presentation,
must not be published as a generic root-post edit. They may update the root's thread-summary widget
and the corresponding `ThreadPostSource` logical count, but they must not make the parent channel
rematerialize that root row.

All viewport position preservation, request look-ahead calculation, materialization and scrolling
remain inside `LongListWidget`. Thread-specific logical identity, edge/cursor choice and demand convergence remain inside `ThreadPostSource`; Mattermost REST transport, pagination encoding, response
normalization and physical HTTP coalescing remain inside the single `PostRepository`.
