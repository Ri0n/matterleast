# Long list: core ownership and layering

> Part of [LongListWidget architecture](../long-list-architecture.md). Read the landing page first and load this file only when this area is relevant.

## Core rule

**Exactly one object owns scroll geometry and the viewport: `LongListWidget`.**

No channel/thread controller, data source, child item widget, cache or navigation service may set a
scrollbar value, calculate pixel positions in the list, create placeholder rows, preserve a viewport
anchor, freeze painting or decide which concrete widgets should exist.

If another class needs something visible, it asks `LongListWidget` in logical-item coordinates.

## Class layering

```text
QAbstractScrollArea
        |
        v
 LongListWidget                    generic geometry + viewport intent
        |
        v
   ChatLogWidget                   Mattermost post UI + semantic post identity
        |
        v
 FilteredPostSource                optional predicate projection
        |
        v
 AbstractPostSource                source/view contract
        |
        v
  IndexedPostSource                raw logical ID slots + structural signals
        |
        +-------------------+
        |                   |
 ChannelPostSource      ThreadPostSource
        |                   |
        +---------+---------+
                  |
          PostTimelineService
                  |
          memory / HTTP / SQLite
```

`FilteredPostSource` is optional: thread timelines currently connect `ChatLogWidget` directly to
`ThreadPostSource`, while the main channel timeline wraps `ChannelPostSource` in a predicate
projection.

`LongListWidget` owns geometry, scrolling, viewport anchoring and persistent logical-item viewport
locks. `ChatLogWidget` owns post-specific presentation, actions and semantic post-ID identity.
`AbstractPostSource` is the view/source interface; `FilteredPostSource` may expose a second
view-facing logical coordinate system without modifying the wrapped source; `IndexedPostSource` owns
the raw transport-agnostic logical ID slot map, exact-window mutation and structural source signals
shared by channel/thread sources. Concrete sources alone decide what server evidence makes a placement
exact and how raw logical demand maps to edge/cursor/page transport. `PostTimelineService` owns
retrieval, physical HTTP request coalescing and cache tiers. See `post-source-architecture.md` for the
complete source-layer contract.

Channel and thread logs share the same widget and therefore the same scrollbar, materialization,
seek, resize and pruning semantics. Their meaningful differences are how logical ranges are
resolved/fetched and how their server-side counts prove sequence boundaries.

## LongListWidget responsibilities

`LongListWidget : QAbstractScrollArea` maintains:

- a logical item count;
- a non-zero default estimated item height;
- an effective height for every logical item;
- availability bits for logical items;
- a bounded set of materialized child `QWidget`s;
- one vertical scrollbar;
- the current ordinary logical viewport anchor;
- an optional persistent logical-item viewport lock;
- random-thumb-seek generation/debounce state;
- pending logical range requests;
- asynchronous item-geometry dirtiness;
- recognition of direct user scroll intent.

It emits logical range requests and never knows what an item represents.

The core API is intentionally small:

```cpp
setItemCount(count);
insertItems(first, count);
setDefaultItemHeight(height);
setRangeAvailable(first, last);
itemsChanged(first, last);
finishRangeRequest(first, last);
scrollToIndex(index, alignment);
scrollToEnd();
lockViewportToItem(index, alignment, quietPeriodMs);
remapViewportLockedItem(index);
clearViewportLock();

signals:
    rangeRequested(first, last, reason, generation);
    visibleRangeChanged(first, last);
    materializedRangeChanged(first, last);
    userViewportChanged(atEnd);
    viewportLockReleased();
```

A subclass supplies a concrete widget for an available logical item through
`createItemWidget(index)`.

## No gap widgets

There are **no placeholder/gap rows**.

For an unavailable or unmaterialized item the list only knows an estimated height. A Fenwick tree
(prefix-sum index) stores effective heights and provides:

```text
item index -> content pixel       O(log N)
content pixel -> item index       O(log N)
height change -> total geometry   O(log N)
```

For example, with 10,000 logical items only ~50 concrete widgets may exist while the scroll range
still represents all 10,000 items.

This removes the old failure mode where giant `QListWidgetItem` gap placeholders participated in
Qt layout and then changed size when real `PostWidget`s were inserted.
