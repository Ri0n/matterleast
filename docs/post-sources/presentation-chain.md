# Post sources: presentation chain and local outgoing augmentation

> Part of [Post source architecture](../post-source-architecture.md). Read the landing page first. This document is the authoritative design for presentation-only timeline augmentation such as optimistic outgoing messages.

## Purpose

MatterLeast has timeline state that is real to the user but is not yet authoritative server history. The primary example is an outgoing message that should appear immediately while its HTTP create request is still pending.

That state must be visible without contaminating the server-backed timeline model.

The design therefore treats optimistic outgoing rows as a **presentation augmentation** layered over an authoritative post source. The same design must work for channel timelines, thread timelines, direct/group chats, and future chat-like timelines. Mattermost's current `(channel_id, root_id)` pair is an adapter for identifying a logical timeline; it is not the generic architecture.

The central ownership rule is:

> authoritative sources own server timeline truth; presentation decorators may project or augment that truth, but must not rewrite it.

## Presentation chain

The intended stack is composable:

```text
authoritative source
    ↓
zero or more projections
    ↓
zero or more presentation augmentations
    ↓
ChatLogWidget / LongListWidget
```

The current MatterLeast stack for ordinary chat is:

```text
ChannelPostSource / ThreadPostSource
    ↓
FilteredPostSource
    ↓
OutboxPostSource
    ↓
ChatLogWidget
```

The order is deliberate.

`FilteredPostSource` removes authoritative rows that should not exist in the presented timeline, such as hidden join/part churn. `OutboxPostSource` then appends local outgoing rows to the already-presented authoritative stream.

Putting the outbox below the filter would make local delivery/retry policy observe raw backend events that the user never sees. Putting the outbox into the authoritative source would pollute paging, counts, navigation, cache and read state.

Future presentation layers should preserve this composability instead of folding unrelated concerns into one large source class.

Every presentation decorator must expose its wrapped source through
`AbstractPostSource::wrappedSource()`. Code that needs the authoritative
channel/thread topology must traverse `authoritativeSource()`; it must never
peel known decorators with concrete `qobject_cast` loops. This is especially
important for permalink/header navigation: fetched context belongs to
`ChannelPostSource`/`ThreadPostSource`, even when the view currently sees
`OutboxPostSource -> FilteredPostSource -> ...`.

## Projection versus augmentation

A **projection** presents a subset/view of wrapped authoritative rows while preserving their semantic identity. `FilteredPostSource` is the current example.

An **augmentation** contributes rows that do not exist in the wrapped authoritative source. The outbox is the current example.

These are different operations and should remain separate:

- projection answers which authoritative rows are visible;
- augmentation answers which additional local rows are visible;
- neither changes what the server source considers authoritative history.

A hidden/rejected authoritative post has no presentation row at all. Do not emulate filtering with one-pixel widgets, zero-height placeholders, `BackendPost::hidden` flags, or other layout tricks.

## Logical timeline identity

Delivery and presentation behavior is scoped to a **logical timeline key**.

The generic concept is opaque: decorators and queueing code need equality/hash semantics, not knowledge of Mattermost endpoint structure.

Current Mattermost mapping:

| Presented timeline | Current key mapping |
| --- | --- |
| channel | backend/account scope + channel ID + empty substream |
| direct message | same as channel; Mattermost represents it as a channel |
| group message | same as channel; Mattermost represents it as a channel |
| thread | backend/account scope + channel ID + root post ID |

Inside the current `PendingPostService`, backend/account identity is provided by the service instance and persistence namespace, so the in-memory queue can use `(channelId, rootId)`. That is an implementation convenience, not a contract future timeline types must copy.

A future source type may provide a different opaque key. Generic presentation code should compare or store that key, not reconstruct channel/thread semantics itself.

## Why this is not a shared fat base class

Channel and thread sources intentionally keep their endpoint-specific count, placement and boundary rules. The same principle applies here.

Genericity should come from narrow composition:

- a timeline key;
- a wrapped `AbstractPostSource`;
- a projection predicate/mapping;
- an additive local-row provider;
- delivery/correlation policy;
- recovery policy.

Do not move channel page arithmetic, thread cursor semantics, server reply counts, or backend-specific transport rules into an augmentation base class merely to make the outbox reusable.

Extract a reusable helper only when it owns an invariant that is truly transport-independent.

## Row ownership and identity

An authoritative row is owned by the wrapped server source.

A local outgoing row is owned by the outbox/presentation service until authoritative confirmation exists.

Every local row needs a stable client identity for its whole unresolved lifetime. For Mattermost this is the `pending_post_id`. The same ID is reused for retries so that:

- the server can deduplicate ambiguous repeated create requests;
- HTTP and realtime confirmation can correlate to the same local row;
- a recovered draft can preserve identity across process restart.

The authoritative server post ID is a different identity.

Presentation semantics on confirmation are a **replacement/promotion**, not two independent messages. The user should not observe a duplicated outgoing message merely because authoritative confirmation arrived through HTTP or WebSocket.

Confirmation is published as one narrow same-index replacement transaction. The wrapped authoritative tail growth and removal of the correlated FIFO pending head are coalesced before the view sees either intermediate cardinality. `LongListWidget::replaceItem()` freezes viewport updates, recreates only the replaced physical row, remeasures it, restores the existing semantic anchor/sticky-bottom intent against final geometry, and paints once. It does **not** use `layoutChanged`.

The transaction must preserve: no duplicate stable row, no intermediate count growth/shrink, no rebuild of unrelated widgets, no viewport jump, and no corruption of semantic navigation.

## Current BackendPost adapter is not authoritative state

The current source API exposes `BackendPost*`. PR #112 therefore builds a synthetic `BackendPost` snapshot for a pending row so existing widgets can render it.

That snapshot is an adapter at the presentation boundary only.

It must never be inserted into:

- `BackendChannel` authoritative collections;
- `PostRepository`;
- persistent post cache;
- server paging slots;
- reply counts;
- read/unread state;
- semantic server navigation.

Architecturally, a future presentation-row interface may make the distinction explicit with row kinds such as authoritative post versus local outgoing row. That would remove the need for a synthetic `BackendPost` adapter. Such an interface is a possible cleanup, not a requirement for every new chat type and not a reason to contaminate backend state today.

## Ordering contract

For one logical timeline, presentation order is:

```text
all visible authoritative rows
all unresolved local outgoing rows in enqueue order
```

The unresolved outbox is therefore a stable tail.

Consequences:

1. a newly arriving authoritative post is inserted **before** the outbox tail;
2. pending/failed rows remain at the end;
3. several local outgoing rows retain FIFO presentation order;
4. removing or confirming one pending row closes only its local slot;
5. server pagination never pages through the local tail.

This rule is independent of whether the underlying authoritative source is a channel, a thread, a DM/GM channel, or another future chat source.

## Structural signal contract

An augmenting source must translate wrapped-source mutations without making the local tail look authoritative.

With a non-empty local tail:

- wrapped insertions before the authoritative end become exact insertions before the tail;
- wrapped removals remove from the authoritative segment only;
- a coarse authoritative count increase must be exposed as an exact structural insertion at the old authoritative boundary so the tail remains anchored;
- local add/remove changes affect only local indices;
- local state changes should emit layout/presentation changes only for the affected local row.

Range requests are split conceptually:

- local rows are immediately available presentation data;
- only the authoritative subrange is forwarded to the wrapped source;
- request completion reported upward still corresponds to the caller's original logical request.

`canRequestBeforeFirst()` and `requestBeforeFirst()` remain properties of the wrapped authoritative history. Local augmentation does not create older server history.

## Viewport and positioning invariants

`LongListWidget` remains the sole owner of pixels, anchors and materialized-widget geometry.

Presentation decorators communicate structural changes; they do not compensate with pixel hacks.

The following existing behavior must survive augmentation:

- ordinary semantic viewport anchors;
- persistent viewport locks used by navigation;
- sticky-bottom behavior;
- scrollbar behavior;
- insertion/removal above a materialized pending row;
- image/layout relayouts;
- sparse history materialization and eviction.

In particular, when a server row arrives while pending rows are visible, the server row must appear before the pending tail without moving the user's semantic reading position unexpectedly.

This is the same reason join/part filtering was implemented as a source projection instead of as tiny hidden post widgets.

## Optimistic tail reveal

Local optimistic insertion has one deliberate viewport-presentation policy above
the source layer. When a newly enqueued pending row is appended at the presented
tail, `ChatLogWidget` asks `LongListWidget` to insert it with
`PreserveVisibleContent` rather than inheriting sticky-bottom immediately.
The pending widget can therefore construct and settle below the current
viewport. `followOwnPost()` then requests `scrollToEndAnimated()`.

This does **not** put animation or pixel knowledge into `OutboxPostSource`.
The outbox still emits only logical `itemsInserted`/availability. The chat view
recognizes that the inserted row is its local pending tail and chooses the
presentation policy; `LongListWidget` owns all pixel distance, animation and
item-visibility translation.

If the user sent while far back in history, `LongListWidget` uses an immediate
tail jump instead of animating across many items.

## Delivery ownership

Presentation and delivery are related but distinct responsibilities.

`OutboxPostSource` presents local rows.

`PendingPostService` currently owns unresolved delivery state, ordering, retries, correlation and durable restart handoff.

`OutgoingPostCreator` creates/enqueues an outgoing operation and releases the composer once the outbox has accepted ownership.

The authoritative post source is not responsible for sending local messages.

This separation lets another chat timeline reuse presentation augmentation without inheriting Mattermost channel paging behavior.

## FIFO semantics

Automatic delivery is FIFO **per logical timeline key**.

Only the head unresolved item may be actively sent/retried. Later items remain visible but blocked behind it.

A failed head does not allow later items to overtake it. The user must retry or cancel the failed head before later rows advance.

Different logical timelines are independent and may make progress concurrently.

This prevents server order from diverging from the order the user entered messages in a given chat.

## Current retry policy

PR #112 currently uses the following bounded automatic policy for ordinary text messages:

- attempt 1 starts immediately;
- at most 6 attempts total;
- total automatic-send window is at most 10 minutes;
- retry delays after failures are approximately 5 s, 20 s, 60 s, 180 s and 300 s, truncated by the remaining 10-minute budget;
- HTTP 408, 425, 429 and 5xx responses are retryable;
- transport/network failure without an HTTP response is retryable;
- other failures become `Failed` immediately.

The important architectural rule is bounded retry, not those exact numeric values. If policy changes, keep it finite and visible to the user.

Retry counters, timers and delivery states are session-only. The durable outbox persists unresolved **message intent**, not transport state, because a process boundary deliberately never resumes automatic delivery. This avoids synchronous disk rewrites on every retry/state transition while keeping the only critical handoff — composer to durable outbox — atomic.

Current presentation states are:

- `Uploading`;
- `Queued`;
- `Sending`;
- `RetryWait`;
- `Blocked` behind an earlier FIFO item;
- `Failed`.

Non-failed pending states do **not** add a textual status row below the message. They use the compact reconnect/busy indicator in the right-side header utility slot immediately before the timestamp — the same slot used by the thread-opening summary on authoritative root posts. A pending post cannot expose the thread action itself because it has no server post ID yet. The indicator reserves the same vertical chip footprint as the eventual thread summary while keeping the spinner itself compact, so ordinary pending -> authoritative confirmation does not change header height. This keeps delivery state away from the author-name geometry and keeps retry/state transitions from perturbing PostWidget height or LongList anchors.

The busy indicator has one process-wide animation/frame provider. Widgets acquire it only while they need animation; the provider runs one timer, caches each rasterized phase/size/palette frame for all consumers, and stops/clears its frame cache when the last consumer releases it. Sidebar reconnect, pending-post indicators and other busy theme buttons use this same clock/cache. Do not introduce per-widget animation timers.

A failed row stops the spinner and exposes the textual failure plus explicit Retry and Cancel actions, because that state requires user attention and interaction. While a row is queued/retrying/blocked, Cancel remains available from the pending post context menu without adding layout height; it is deliberately unavailable while an HTTP create is already in flight.

## Conversation-advanced cutoff

Automatic retry must stop when the presented conversation has clearly moved on.

Current policy: after **3 newer visible authoritative messages** in the same logical timeline, no further automatic retry is started for the head pending row.

"Visible" is essential. This is a presentation rule, not a raw backend-event counter.

For the main channel timeline, hidden join/part events do not consume this budget because `FilteredPostSource` would not present them.

For another timeline/projection, the equivalent visibility policy is supplied by that timeline to the delivery service. The delivery layer does not hard-code channel event types.

A confirmation of one of our own pending rows is expected FIFO progress and must not count as an intervening message against later queued rows.

### In-flight request at the cutoff

An HTTP POST that has already been dispatched cannot be safely made nonexistent just because the third visible message arrived.

Therefore:

- mark that no future automatic retry is allowed;
- let the current request complete;
- if it succeeds, confirm normally;
- if it fails, transition to `Failed` without scheduling another automatic attempt.

Do not fake cancellation of a request that may already have reached the server.

## Manual Retry and Cancel

Manual Retry represents a new user decision.

It resets the automatic attempt budget, the 10-minute window and the intervening-visible-message counter while retaining the same stable client correlation ID.

Keeping the same correlation ID preserves idempotency if an earlier ambiguous attempt actually reached Mattermost.

Cancel removes only that unresolved local item and allows the next FIFO item in the same logical timeline to proceed.

Manual actions must not mutate authoritative server history.

## HTTP and realtime correlation

Confirmation may arrive through more than one path:

- the HTTP create response;
- a realtime/WebSocket post carrying the same client correlation ID.

Whichever valid confirmation is observed first resolves the local row.

Later duplicate confirmation paths must be harmless.

The outbox must never depend on a particular arrival order between HTTP completion and realtime delivery.

## Read state, counts, paging and navigation

Local outgoing rows are presentation data, not server topology.

They do **not** participate in:

- read cursor advancement;
- unread calculation;
- thread reply count;
- channel authoritative counts;
- sparse logical server slots;
- server pagination;
- "load before/after" decisions;
- permalink/semantic server navigation;
- `PostRepository`;
- durable server-post cache;
- Following/Attention server projections.

A future feature may intentionally build a local-only projection over outbox rows, but that must be explicit. Never let a pending row accidentally masquerade as an authoritative server post merely because existing UI accepts `BackendPost*`.

## Restart boundary

Process restart is a hard boundary for automatic delivery.

Unresolved messages from the previous process are **never sent automatically on startup**.

Rationale:

- the user may have restarted specifically because networking or sending behaved incorrectly;
- an ambiguous prior HTTP request may already have reached the server;
- automatic replay after a long process boundary is more surprising than preserving an editable recovery record.

Instead, durable unresolved outbox entries are converted into local recovered drafts.

## Recovered drafts

Each unresolved outbox item becomes its own editable recovered draft.

Several pending messages may belong to the same logical timeline, so ordinary "one draft per conversation" identity is insufficient. The current implementation adds:

```text
recoveryId = pending_post_id
```

to the local draft key.

This guarantees that:

- two unsent messages from the same channel/thread remain two drafts;
- an existing ordinary draft for that conversation is not overwritten;
- opening one recovered draft edits exactly that recovered item;
- sending/deleting it removes exactly that recovery entry.

Recovered drafts are local-only and must not be synchronized into Mattermost's one-draft-per-conversation server API.

Ordinary composer drafts use a separate persistence contract: the 600 ms typing debounce writes only the local DraftStore for crash safety. Mattermost remote draft create/update is requested when leaving or closing that composer (or explicitly switching away from its draft identity). Sending removes a never-published local draft without a remote create/delete round trip; a previously published remote draft is deleted normally.

For quoted replies, recovery preserves the reply target and strips the generated fallback quote from the editable message body rather than forcing the user to edit wire-format fallback text.

Recovered records leave the outbox only after their individual DraftStore handoff is durable. If conversion stops on a persistence failure, only the failed/unprocessed suffix remains as a recovery backlog and is merged into later live outbox snapshots. Already-durable recovered drafts are not retained in that backlog, so sending or deleting one in the current process cannot make it reappear on a later restart.

There is one deliberate deduplication exception to preserving an ordinary draft in the same conversation: a crash may occur after the outbox handoff commits but before the sent composer's ordinary draft is removed. After the recovered entry is durable, an ordinary draft is discarded only when its message and quoted-reply target exactly match the recovered record. A differing ordinary draft is preserved.

## Account isolation

Durable local state must not cross server/account boundaries.

The current implementation namespaces persisted outbox state using the backend host plus logged-in user identity. Generic future implementations need the same property even if their timeline key itself does not contain account information.

Never correlate or recover pending rows solely by channel/root IDs across accounts.

## Attachment upload ownership

Attachment-bearing sends use the same outbox ownership boundary as text messages.

Selecting a file may start a backend-scoped pre-upload immediately so normal
composition latency can overlap with network transfer. The composer does not own
that transfer. `AttachmentUploadService` owns the upload by a stable local
attachment ID, and `PendingPostService` takes over that same ID when Send is
pressed.

Consequences:

- Send remains available while a pre-upload is running;
- pressing Send immediately creates the optimistic outbox row and releases the
  composer;
- the pending row remains in `Uploading` until every attachment has a server
  `file_id`, then normal FIFO post creation begins;
- a failed upload fails that outbox item and exposes Retry/Cancel;
- later messages in the same logical timeline cannot overtake an uploading or
  failed head;
- removing an attachment before Send releases only the staged upload identity,
  not authoritative post state.

The process restart boundary is unchanged: no old network operation is resumed
automatically. Durable outbox records persist local attachment paths, and startup
recovery places those paths in the recovered local draft. Restoring that draft
stages the files again; if a local file no longer exists, the recovered operation
remains editable rather than silently sending a text-only message.

Recovered Draft attachment paths are local-only. They are never synchronized to
Mattermost's one-draft-per-conversation API, which has no representation for
local upload intent.

## Source lifetime

The augmentation must tolerate the wrapped authoritative source changing or being destroyed without transferring ownership of authoritative posts.

The local service owns unresolved local rows independently of the materialized chat widget/source instance.

Closing a thread window, switching channels or destroying a source must not implicitly cancel a pending send.

When that logical timeline is presented again during the same process, the augmentation can reconstruct its local tail from the service's unresolved items.

## Generic integration contract for another chat source

A new chat-like timeline should need only the following integration points:

1. provide an authoritative `AbstractPostSource`;
2. provide an opaque logical timeline key;
3. apply any authoritative visibility projection before local augmentation;
4. expose local pending rows for that key;
5. define how authoritative confirmation carries the client correlation ID;
6. define which authoritative advances count as "visible newer messages" for retry cutoff;
7. provide a recovery target capable of losslessly representing the unresolved outgoing operation.

It should **not** have to:

- emulate channel page counts;
- emulate thread cursors;
- insert pending rows into backend/server collections;
- teach `LongListWidget` about outbox-specific pixels;
- add hidden placeholders to preserve indices.

## Current #112 mapping

The current implementation is a first concrete adapter of this design:

| Generic concept | Current implementation |
| --- | --- |
| logical timeline key | `(channelId, rootId)` inside one `Backend` |
| authoritative source | `ChannelPostSource` or `ThreadPostSource` |
| visibility projection | `FilteredPostSource` |
| augmentation | `OutboxPostSource` |
| local delivery owner | `PendingPostService` |
| client correlation ID | Mattermost `pending_post_id` |
| rendering adapter | synthetic presentation-only `BackendPost` snapshot |
| restart recovery | local `DraftService` recovered entry with `recoveryId` |
| attachment behavior | backend-scoped pre-upload handed to `PendingPostService` on Send |

These names are implementation facts, not requirements future chat types must reproduce.

## Known genericization pressure points

The current implementation still has Mattermost-specific seams that should not be mistaken for final generic API:

### `channelId/rootId` constructor parameters

`OutboxPostSource` currently selects rows using channel/root strings. If another timeline type cannot naturally map to that pair, replace this with an opaque timeline key/provider rather than inventing fake channel/root values.

### visibility cutoff observation

The presentation layer supplies a per-timeline visibility predicate to `PendingPostService`. This keeps hidden-event policy above the backend delivery machinery while still allowing cutoff tracking when the chat view is not materialized.

### generic source authority metadata

`AbstractPostSource` exposes whether a view-facing row is authoritative, how many authoritative rows are present, and whether a semantic post has a server-confirmed logical position. Decorators delegate the position-authority question to their wrapped source. Read state, bookmarks and selection can therefore exclude local augmentation without depending on `OutboxPostSource` specifically.

### synthetic `BackendPost`

This exists because current widgets/source APIs render `BackendPost*`. If more non-authoritative row types appear, consider a first-class presentation-row abstraction. Do not solve the issue by admitting synthetic posts into backend authority.

### confirmation transaction

The semantic contract is one pending row becoming one authoritative row with stable viewport behavior. If future source APIs gain an explicit replace/identity-transition signal, prefer that over exposing a transient duplicate. Until then, structural insert/remove must preserve equivalent visible semantics.

## Rejected approaches

### Insert pending messages into BackendChannel/PostRepository

Rejected because it makes local uncertainty look like server truth and contaminates counts, cache, paging, navigation and read state.

### Hide unwanted rows in the widget layer

Rejected for the same reason join/part filtering moved to `FilteredPostSource`: one-pixel/zero-height rows preserve the wrong logical model and destabilize positioning.

### Reuse backend hidden flags

Rejected because filtering/presentation policy is not a property of the authoritative post object. The same post may be present in one projection and absent in another.

### Merge filtering and augmentation into one special chat source

Rejected because the operations compose differently and should be reusable independently.

### Automatically resend durable outbox entries after restart

Rejected because restart is a user-visible uncertainty boundary and an earlier request may already have succeeded.

### Let later local messages overtake a failed/slow head

Rejected because it can reverse the user's send order in a single logical conversation.

### Make channel/thread transport rules generic merely for reuse

Rejected. Share only transport-independent invariants; keep endpoint-specific topology in concrete sources.

## Regression matrix

Changes to this subsystem should cover the relevant items below.

### Projection and ordering

- hidden authoritative rows create no presentation slot;
- authoritative rows arriving with a non-empty local tail insert before that tail;
- several pending rows keep enqueue/FIFO order;
- two logical timeline keys progress independently;
- channel and thread adapters obey the same augmentation contract.

### Correlation

- HTTP confirmation resolves the pending row;
- realtime confirmation resolves the same row;
- HTTP/realtime race cannot create duplicate authoritative presentation;
- own confirmation does not consume the three-message cutoff for later pending rows.

### Retry

- retryable failure schedules bounded retry;
- non-retryable failure becomes `Failed`;
- third newer visible authoritative message stops future auto-retry;
- hidden join/part churn does not consume the cutoff;
- third visible message during an in-flight POST allows that request to finish but schedules no next retry;
- failed FIFO head blocks later messages;
- Retry resets the user-approved budget without changing the client correlation ID;
- Cancel advances the next FIFO item.

### Authority isolation

- pending rows do not change read cursor;
- pending rows do not change reply count;
- pending rows do not change server pagination/counts;
- pending rows are absent from `PostRepository` and durable server-post cache;
- semantic server navigation does not treat a pending ID as a server post.

### Restart recovery

- startup never auto-sends an unresolved old entry;
- one unresolved entry becomes one recovered draft;
- multiple unresolved entries in the same timeline become distinct recovered drafts;
- an ordinary draft for that timeline remains intact;
- editing/sending/deleting one recovered draft affects only that entry;
- quoted-reply recovery restores editable text and reply target;
- recovery is account-isolated and idempotent.

### Viewport stability

- incoming authoritative insertion before a materialized pending tail preserves semantic anchor;
- confirmation does not jump the viewport;
- normal sticky-bottom remains sticky; optimistic own-tail insertion may deliberately preserve the
  old visible content until its explicit animated follow;
- pending state relayout changes only its own row;
- paging/materialization around an outbox tail does not invent server slots.

### Attachments

- Send stays enabled for a new message while attachment pre-upload is active;
- Send immediately transfers upload ownership to the outbox and clears the composer;
- an uploading FIFO head blocks later local messages in that logical timeline;
- upload completion supplies `file_id` values before post creation starts;
- upload failure produces an explicit failed outbox row with Retry/Cancel;
- restart recovery restores local attachment paths into the recovered draft and never auto-sends.

## Documentation rule

When this design evolves, update this document with the architectural invariant and update concrete source/delivery documents only with their adapter-specific behavior.

Do not duplicate the full outbox design into channel, thread or widget documentation. Those documents should link here and retain only the invariants they own.
