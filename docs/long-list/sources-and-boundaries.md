# Long list: sources, cache boundary and migration

> Part of [LongListWidget architecture](../long-list-architecture.md). Read the landing page first and load this file only when this area is relevant.

## Post sources

The preferred boundary is a small source abstraction used by `ChatLogWidget`:

```cpp
class AbstractPostSource {
public:
    virtual int itemCount() const = 0;
    virtual bool isAvailable(int index) const = 0;
    virtual BackendPost* postAt(int index) const = 0;
    virtual QString postIdAt(int index) const = 0;
    virtual int indexOfPost(const QString& postId) const = 0;
    virtual int ensurePostIndex(const QString& postId);
    virtual void requestRange(int first, int last, RequestReason, quint64 generation) = 0;
    virtual bool canRequestBeforeFirst() const;
    virtual void requestBeforeFirst(RequestReason, quint64 generation);

signals:
    itemCountChanged(count);
    itemsInserted(first, count);
    itemsRemoved(first, count);
    rangeAvailable(first, last);
    bodyAvailabilityChanged(first, last, available);
    seekTargetResolved(index, generation);
    layoutChanged(first, last);
    rangeRequestFinished(first, last);
};
```

`postIdAt(index)` is stable semantic identity independent of resident body availability. That is what
allows view-model decorators to keep an acceptance/rejection decision while a `BackendPost` body is
evicted.

`ChannelPostSource` and `ThreadPostSource` adapt different Mattermost endpoints into the same raw
logical contract. Their common index-to-ID bookkeeping lives in `IndexedPostSource`; channel count
repair, thread root/cursor semantics and endpoint-specific boundary proofs remain in the concrete
source. `FilteredPostSource` can wrap any `AbstractPostSource` with an arbitrary
`std::function<bool(const BackendPost&)>` predicate and translate structural/data signals into a
second contiguous coordinate space. A small shared `PostSourceRequestGate` handles only attachment of
compatible logical waiters to one exact in-flight boundary request. None of these source objects
manipulate widgets or scrollbars.

`BackendPost::hidden` is a channel/thread topology concern: replies are intentionally marked hidden
from the raw channel-root sequence. It must not be repurposed for presentation filtering.
`ThreadPostSource` still exposes posts whose `root_id` matches its thread.

Optional presentation filtering belongs in a source projection such as `FilteredPostSource`. A
rejected post has no `LongListWidget` row at all; it is not represented by a zero-height/one-pixel
widget and it is not deleted from the wrapped source.

Normal open policy also belongs outside geometry:

```text
ordinary channel open -> ChatLogWidget::scrollToEnd()
ordinary thread open  -> ChatLogWidget::scrollToEnd()
permalink/Attention/Recent -> semantic post-ID identity + LongListWidget viewport lock
```

### Exact versus unknown logical count

`LongListWidget::itemCount()` is the source's current logical coordinate space, not spare UI capacity.
For threads, `reply_count + 1` normally gives a reliable logical size because deleted replies are
excluded from `ReplyCount`.

For channels, `total_msg_count_root` is only an **estimate** used to initialize the oldest-to-newest
coordinate space. It can be too small because count-excluded system roots are returned by `/posts`, or
too large because soft-deleted normal roots remain represented in the historical counter. Empty slots
in that estimated coordinate space are therefore unresolved source positions, not proof that a
particular absolute index is already authoritative.

Some Mattermost versions do not provide `total_msg_count_root`. A cached newest suffix is then **not**
an exact complete channel and must not silently become one, otherwise index 0 falsely looks like the
oldest edge and older history can never be requested.

Unknown-count mode therefore uses a contiguous sequence of **actually discovered root posts**. When
the visible window reaches its oldest edge, `ChannelPostSource` requests a cursor page before the
first known post. New real rows are prepended through:

```text
PostSource::itemsInserted(0, count)
        -> LongListWidget::insertItems(0, count)
        -> shift logical indices / Fenwick heights / materialized map
        -> restore Bottom, persistent lock, or ordinary anchor
```

No spare logical capacity and no fake placeholder rows are introduced. Outstanding view-side pending
range bits are discarded on a structural index shift; repository-level physical HTTP coalescing still
prevents duplicate equivalent transport work. If the server reports no older cursor/data, the source
marks the oldest boundary reached and stops repeating that request.

Do not substitute `total_msg_count` for the missing root count: it may include thread replies and
would create phantom logical rows.

## Cache boundary

`PostTimelineService` remains below sources and is responsible for:

1. already present BackendChannel data;
2. equivalent physical in-flight HTTP request coalescing;
3. future SQLite post cache;
4. HTTP for the remaining missing data.

Logical request planning and attachment are intentionally above that layer in the concrete post
sources because they depend on source indices, known edges and current viewport demand.

A successful stale request is still useful cache population. Cache success and viewport authority
are deliberately separate concepts.

## Prohibited ownership outside LongListWidget

New production code outside `LongListWidget` must not call, for a chat log:

```text
verticalScrollBar()->setValue(...)
connect to scrollbar signals to infer chat viewport intent
override wheel handling to implement chat scrolling
scrollToBottom()
scrollToItem(...)
setUpdatesEnabled(false/true) for list transactions
doItemsLayout() for chat-log geometry
calculate gap height / pixel seek position
store a pixel viewport anchor or navigation offset
create a placeholder item representing missing posts
```

`ChatLogWidget` may request logical operations from its base (`scrollToIndex`, `scrollToEnd`,
`lockViewportToItem`, `remapViewportLockedItem`) but must not bypass those APIs to manipulate pixels.

A temporary migration adapter that needs one of the prohibited operations must live under
`deprecated/` and must not be linked into the final target.

## Migration from deprecated sparse timeline

The old implementation is moved under `deprecated/` and excluded from normal source globbing. This
is intentional: compile errors are the migration checklist.

Old responsibility -> new owner:

| Deprecated responsibility | New owner |
| --- | --- |
| `PostTimeline` gap spans / pixel mapping / measured heights | `LongListWidget` height index |
| `TimelineSeekState` | `LongListWidget` internal seek state |
| channel/thread viewport checks | `LongListWidget` |
| channel/thread pruning | `LongListWidget` materialization budget |
| `PostsListWidget` sparse pixel-anchor restore | `LongListWidget` |
| `PostsListWidget` semantic post navigation identity | `ChatLogWidget` post-ID identity |
| `PostsListWidget` semantic viewport position lock | `LongListWidget` persistent viewport lock |
| `ResizableListWidget` chat-row size anchoring | `LongListWidget` child event filter |
| channel/thread HTTP adapters | post sources / `PostTimelineService` |
| PostWidget-specific actions | `ChatLogWidget` |

The migration should remove dependencies rather than add compatibility wrappers back to deprecated
headers.

## Required tests before Mattermost integration

`LongListWidget` is tested first with a synthetic source, without Mattermost objects:

- 10,000 uniform items: middle scrollbar position maps near item 5,000;
- missing ordinary viewport items are requested as contiguous logical demand rather than arbitrary
  transport blocks;
- random unavailable thumb seek uses a bounded seed window;
- no gap/placeholder widgets exist;
- materialized QWidget count never exceeds the configured budget;
- delayed sizeHint growth above the viewport preserves the logical anchor;
- delayed sizeHint growth at bottom preserves the true newest edge;
- logical item-count growth while sticky-bottom is active follows the new end;
- prepend shifts logical indices without moving the existing viewport;
- prepend preserves sticky-bottom on the same newest logical item;
- a persistent viewport lock keeps the same absolute item Y through reflow;
- a persistent viewport lock keeps the same relative Y through viewport resize;
- remapping a persistent viewport lock to a new logical index preserves screen position;
- arbitrary late reflow cannot leave the viewport with no materialized items;
- window resize preserves the end and immediately updates reachable scrollbar range;
- random seek uses normalized logical target and 100 ms debounce;
- stale seek availability cannot move a newer viewport;
- seed measurement expands only enough to cover viewport plus buffer.

Domain integration additionally needs tests that:

- an ordinary channel/thread open at the newest edge can satisfy a multi-block-sized desired range
  with one demand-sized edge request;
- a following adjacent range continues from an authoritative before/after cursor;
- an overlapping/adjacent request can attach to compatible in-flight boundary work while a distant
  fast-scroll request remains independent;
- a provisional permalink target can move to an authoritative index without moving the semantic
  viewport away from that post;
- clearing a provisional source slot clears list availability even if no widget existed there;
- cached and live thread replies remain visible despite the channel-root `hidden` flag;
- a server without `total_msg_count_root` can discover older root posts without fake UI rows;
- predicate filtering removes model rows rather than creating placeholder widgets;
- filtering a row immediately before a viewport-locked target remaps the target index while preserving
  the exact target widget and screen Y;
- filtered range requests and insert/remove/layout signals translate correctly between view-facing and
  raw source coordinates.
