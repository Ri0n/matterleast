# Post cache: admission and persistence

> Part of [Post cache architecture](../post-cache.md). Read the landing page first and load this file only when this area is relevant.

## Goals

The cache exists to make already-seen history cheap to revisit and cheap to restore after restart,
without turning the in-process `BackendChannel` model into an unbounded second copy of server
history.

The data path is:

```text
ChannelPostSource / ThreadPostSource
                |
                v
        PostTimelineService
       (PostRepository today)
                |
        +-------+-------+
        |               |
 resident posts    PostCacheService
                        |
                 dedicated QThread
                        |
                  PostCacheStore
                        |
                      SQLite
        |               |
        +-------+-------+
                |
              HTTP
```

The current `PostTimelineService` name is still a compatibility alias for `PostRepository`. The
cache is introduced at that boundary first; the mechanical rename can happen separately without
changing cache semantics.

## Channel admission policy

Mattermost can deliver live post events for many joined channels even when the user rarely opens
most of them. Message activity therefore must **not** be treated as cache interest: otherwise a busy
channel can keep itself permanently hot in both RAM and SQLite without any reading gesture from the
user.

Cache interest is driven only by the most recent **channel open** observation:

- resident-memory admission horizon: **1 hour** by default;
- persistent-disk admission horizon: **10 hours** by default, approximately one working day;
- opening a thread counts as opening its parent channel at that moment;
- notification/permalink/Attention navigation counts once it actually opens the channel;
- incoming posts, mentions, unread state, typing and reactions never refresh cache interest;
- the currently open channel is always hot because activation records a fresh open observation.

The existing Mattermost `channel_open_time` preference is a useful startup seed. It lets the client
recover recent channel interest across process restarts. A local `ChatArea` activation updates the
same policy immediately rather than waiting for a preference round trip. `channel_open_time` is
preferred over `last_post_at`, `last_viewed_at` or generic activity timestamps because those can move
without a deliberate local reading gesture.

The two horizons are **admission and retention gates**, not replacements for LRU/TTL limits:

```text
channel opened <= 1 h ago
        -> eligible for resident materialization
channel opened <= 10 h ago
        -> eligible for durable post storage
older / never opened
        -> metadata/unread processing only; no general post-body cache admission
```

An open thread has one deliberately narrower lifetime rule. `ThreadPostSource` leases its root post.
If the parent channel later ages past the one-hour resident horizon while that thread remains open,
a WebSocket `posted` reply whose `root_id` matches the leased root is still admitted into
`BackendChannel` so the open thread can consume the live body through its normal `onNewPost` signal.
This is **not** a channel-interest refresh: unrelated posts in that cold channel remain transient, the
channel-open timestamp does not move, and disk admission still follows the normal ten-hour policy.

A channel crossing the one-hour memory horizon makes all of its unleased resident posts immediately
eligible for eviction even if the channel is busy. A channel crossing the ten-hour disk horizon is
removed from persistent post storage during cache maintenance. Global byte/count LRU remains a
secondary pressure rule among still-eligible channels.

Persistent channel-interest metadata belongs in the cache database so expiry can be enforced by the
worker without depending on UI lifetime:

```text
channel_usage
    account_id
    channel_id
    last_opened_at

PRIMARY KEY(account_id, channel_id) WITHOUT ROWID
INDEX(account_id, last_opened_at)
```

Like posts themselves, channel usage is account-scoped. A visit from account A must never admit rows
for account B.

## Persistent store

All accounts use one SQLite database under `QStandardPaths::CacheLocation`. Rows are isolated by a
small integer `account_id` derived from `(server, Mattermost user id)` so secondary indexes do not
repeat a long server URL in every entry.

Post payloads remain Mattermost JSON rather than a duplicated normalized object schema. The payload
is encoded with `QJsonDocument::Compact` and compressed before storage. Only fields required for
lookup, ordering and eviction are indexed separately:

```text
accounts
    id INTEGER PRIMARY KEY
    server TEXT
    user_id TEXT

posts
    account_id
    post_id
    channel_id
    root_id
    create_at
    update_at
    last_access
    payload BLOB

PRIMARY KEY(account_id, post_id) WITHOUT ROWID
INDEX(account_id, channel_id, root_id, create_at, post_id)
INDEX(last_access)

channel_usage
    account_id
    channel_id
    last_opened_at

PRIMARY KEY(account_id, channel_id) WITHOUT ROWID
INDEX(account_id, last_opened_at)
```

`root_id == ''` identifies channel roots. A non-empty `root_id` identifies replies in that thread.
Attachments themselves are not stored here; only the post JSON and attachment metadata already
contained in it are cached.

The cache is intentionally not encrypted at rest in this first implementation. Account scoping
prevents accidental cross-account reuse inside the client, but it is not filesystem encryption.

## SQLite thread ownership and asynchronous writes

SQLite work must never block the UI/network callback thread. `PostCacheStore` is deliberately a
synchronous, thread-confined object; `PostCacheService` is the asynchronous boundary.

`PostCacheService` owns one dedicated `QThread`. The worker lazily constructs `PostCacheStore` from
inside that thread, so the `QSqlDatabase` connection and the store's periodic maintenance timer are
created, used and destroyed on the same thread. `PostCacheStore` is never constructed on one thread
and then moved with a live SQL connection.

Every queued operation carries its complete account key:

```text
(server, Mattermost user id, operation payload)
```

The worker selects that account immediately before executing the operation. There is intentionally
no mutable "current account" on the calling thread: a queued write produced for account A must still
go to account A even if the UI logs out and selects account B before SQLite processes it.

For REST requests the account key is captured when the repository starts the logical request, not
when the response callback eventually runs. For WebSocket events it is captured when the event is
handled. This prevents an old delayed response from being filed under a newly selected account.

Writes are **write-behind physically, write-through logically**:

```text
successful server payload
        |
queue durable cache command
        |
update resident model / deliver callback
```

The UI does not wait for an SQLite commit because the cache is disposable and never server
authority. A process crash between queueing and commit may lose cache warming, but cannot lose user
or server state. Normal `PostCacheService` destruction drains all earlier queued commands, runs a
final maintenance pass and destroys the SQL connection on the worker thread before joining it.

## Causal ordering of cache writes

A response is not necessarily fresh merely because it arrives late. For example:

```text
HTTP request A starts
        |
WebSocket delete/reaction is observed
        |
cache row is invalidated
        |
old HTTP response A arrives
```

The final response must not resurrect a snapshot that predates the WebSocket mutation.
`PostRepository` therefore assigns a monotonic observation sequence per backend. The sequence is
captured by the **physical HTTP request** when it is dispatched and by each WebSocket cache
mutation when it is observed. Request coalescing shares that original request sequence; a later
caller joining an existing request cannot make the in-flight response appear newer.

The cache worker keeps a short-lived `(account, post_id) -> invalidation sequence` watermark. A
queued store whose source observation is older than or equal to a newer invalidation is discarded
for that post. Invalidations themselves are never suppressed by channel-admission policy because a
known-stale row must be removable even after its channel has gone cold.

The same ordering concept is also applied to resident refresh. Each physical HTTP request keeps
its dispatch observation sequence, while WebSocket new/edit/delete/reaction observations advance the
resident watermark immediately. `PostRepository` drops an older response per post before it reaches
`BackendChannel`, so stale REST work cannot overwrite a newer WebSocket state in memory. Reply
observations conservatively fence their root ID too because ingesting a reply can update transient
thread metadata on the root. These watermarks are short-lived in-flight causality guards, not
persistent cache freshness metadata.

## Disk limits and compaction

Persistent limits are global across the SQLite file unless stated otherwise:

- only channels opened within **10 hours** are eligible by default;
- at most **10,000 posts** total;
- at most **1,000 cached replies per thread**;
- at most **5 GiB** of compressed post payloads;
- LRU eviction by `last_access` among still-eligible rows;
- 5% hysteresis after crossing a global limit to avoid one-row eviction churn.

Row limits are enforced during writes, not only at application shutdown.

SQLite uses:

```text
page_size=4096
journal_mode=WAL
synchronous=NORMAL
auto_vacuum=INCREMENTAL
```

A very-coarse maintenance timer runs every ten minutes on the cache worker thread even when the
cache receives no lookups. A maintenance pass:

1. removes post rows for channels outside the configured disk-interest horizon;
2. re-enforces per-thread and global LRU limits;
3. executes `PRAGMA optimize`;
4. checkpoints and truncates WAL;
5. runs bounded incremental vacuum when at least 128 pages and 5% of the database are free.

Incremental vacuum is deliberate. Full `VACUUM` can temporarily require another database-sized file
and can block for a long time on a multi-gigabyte cache. Reclaiming at most 4096 pages per pass keeps
the single SQLite file compact without creating a second ~5 GiB working copy.
