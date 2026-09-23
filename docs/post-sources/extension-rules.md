# Post sources: extension rules

> Part of [Post source architecture](../post-source-architecture.md). Read the landing page first and load this file only when this area is relevant.

## What is intentionally *not* shared

The following concepts must not move into `IndexedPostSource`, `ChatLogWidget` or `LongListWidget`:

| Concept | Owner |
| --- | --- |
| channel page-zero newest-edge bootstrap | `ChannelPostSource` |
| disconnected `/posts?page=N` mapping | `ChannelPostSource` |
| `total_msg_count_root` adjustment | `ChannelPostSource` |
| oldest exponential/binary page probing | `ChannelPostSource` |
| provisional channel navigation island | `ChannelPostSource` |
| thread root at index 0 | `ThreadPostSource` |
| thread `fromPost/fromCreateAt` cursors | `ThreadPostSource` / repository transport |
| thread initial/tail authority | `ThreadPostSource` |
| short-lived adjacent demand attachment | shared `PostSourceRequestGate` helper |
| predicate-based view projection / filtered index mapping | `FilteredPostSource` |
| scrollbar, pixels, viewport anchor | `LongListWidget` |
| semantic post-ID navigation lock | `ChatLogWidget` + `LongListWidget` lock |
| physical HTTP coalescing / snapshot freshness | `PostRepository` / `PostCacheService` |

Sharing endpoint-specific count and placement rules would make the common base depend on one Mattermost
endpoint and eventually force the other source to emulate semantics it does not have.

## Cache interaction

Sources store IDs, not durable `BackendPost*` pointers. `postAt()` resolves an available ID through the
current `BackendChannel` resident map each time. This is required for the planned bounded resident
cache: a post body may eventually be evicted and later rematerialized from SQLite/HTTP without changing
its logical identity.

Cached post data has weaker authority than server transport placement:

- a direct cache hit may make an identity resident;
- a cached newest channel/thread suffix may be used for provisional first paint;
- SQLite timestamps/ordering do not prove an absolute channel page or thread cursor adjacency;
- HTTP/thread boundary results are still required before a provisional window becomes authoritative.

See `post-cache.md` for snapshot freshness and resident/durable ownership.

## Extension rules

When adding a new source behavior, decide in this order:

1. **Does it manipulate pixels, scrollbar or materialized widgets?** It belongs in `LongListWidget`.
2. **Does it present or navigate by semantic post ID?** It belongs in `ChatLogWidget`.
3. **Does it expose a predicate-filtered logical projection while leaving transport truth unchanged?**
   It belongs in `FilteredPostSource` or another `AbstractPostSource` decorator.
4. **Is it pure raw logical ID/slot bookkeeping independent of Mattermost transport?** It belongs in
   `IndexedPostSource`.
5. **Is it generic short-lived attachment of logical demand to an exact in-flight boundary request?**
   It belongs in `PostSourceRequestGate`.
6. **Does it prove where a result belongs using channel pages or thread cursors?** It stays in the
   concrete source.
7. **Does it retrieve/cache/fence post snapshots or coalesce equivalent physical HTTP?** It belongs
   below the sources in `PostRepository`/`PostCacheService`.

The goal is not maximum inheritance. The goal is one owner for each invariant.
