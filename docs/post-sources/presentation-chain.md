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

The ideal presentation transaction keeps count/anchor behavior equivalent to replacing the pending row with the authoritative row. With the current `AbstractPostSource` interface this may be implemented as authoritative insertion immediately before the local tail followed by removal of the correlated pending row. That structural implementation must still preserve the same visible semantics: no duplicate stable row, no viewport jump, and no corruption of semantic navigation.

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

Current presentation states are:

- `Queued`;
- `Sending`;
- `RetryWait`;
- `Blocked` behind an earlier FIFO item;
- `Failed`.

A failed row exposes explicit Retry and Cancel actions.

## Conversation-advanced cutoff

Automatic retry must stop when the presented conversation has clearly moved on.

Current policy: after **3 newer visible authoritative messages** in the same logical timeline, no further automatic retry is started for the head pending row.

"Visible" is essential. This is a presentation rule, not a raw backend-event counter.

For the main channel timeline, hidden join/part events do not consume this budget because `FilteredPostSource` would not present them.

For another timeline/projection, the equivalent visibility policy must be supplied by that timeline rather than hard-coding channel event types into a generic outbox abstraction.

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

For quoted replies, recovery preserves the reply target and strips the generated fallback quote from the editable message body rather than forcing the user to edit wire-format fallback text.

The durable outbox file is removed only after valid records have been handed to the draft service's own durable persistence, so recovery remains idempotent across an interrupted startup conversion.

## Account isolation

Durable local state must not cross server/account boundaries.

The current implementation namespaces persisted outbox state using the backend host plus logged-in user identity. Generic future implementations need the same property even if their timeline key itself does not contain account information.

Never correlate or recover pending rows solely by channel/root IDs across accounts.

## Attachments are intentionally excluded for now

PR #112 keeps attachment-bearing sends on the existing acknowledged-send path.

The reason is recovery, not rendering.

An uploaded attachment has server-side `file_id` state, but the current editable Draft model cannot faithfully reconstruct the attachment intent from those IDs after restart. Restoring only the text would silently lose part of the message.

Therefore the optimistic durable outbox is currently text-only.

Do not extend optimistic enqueueing to attachments until the recovery representation can restore everything required to edit/resend the message safely.

Persisting `file_ids` in an outbox record by itself is not sufficient if the recovered composer cannot represent them.

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
| attachment behavior | existing acknowledged-send path |

These names are implementation facts, not requirements future chat types must reproduce.

## Known genericization pressure points

The current implementation still has Mattermost-specific seams that should not be mistaken for final generic API:

### `channelId/rootId` constructor parameters

`OutboxPostSource` currently selects rows using channel/root strings. If another timeline type cannot naturally map to that pair, replace this with an opaque timeline key/provider rather than inventing fake channel/root values.

### visibility cutoff observation

`PendingPostService` currently knows enough about the main channel filter to exclude hidden membership churn. The generic direction is to observe "authoritative rows visible in this presentation" through a narrow policy/callback or source-layer event, not to accumulate more hard-coded post types inside the delivery service.

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
- sticky-bottom remains sticky when appropriate;
- pending state relayout changes only its own row;
- paging/materialization around an outbox tail does not invent server slots.

### Attachments

- attachment-bearing messages stay on acknowledged-send behavior until lossless recovery exists.

## Documentation rule

When this design evolves, update this document with the architectural invariant and update concrete source/delivery documents only with their adapter-specific behavior.

Do not duplicate the full outbox design into channel, thread or widget documentation. Those documents should link here and retain only the invariants they own.
