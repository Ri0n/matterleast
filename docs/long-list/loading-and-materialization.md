# Long list: loading and materialization

> Part of [LongListWidget architecture](../long-list-architecture.md). Read the landing page first and load this file only when this area is relevant.

## Request demand policy

Pixel motion is not source demand. `LongListWidget` caches the last synchronized logical viewport
window and ordinary scrollbar movement schedules synchronization only when the newly computed desired
window differs.

This cache does **not** suppress semantic triggers. Source availability/progress, structural mutation,
height changes, viewport resize and explicit navigation may resynchronize the same logical range
because what can be materialized or how it fits may have changed even when its indices did not.


Ordinary viewport/prefetch demand is not rounded to a fixed block size by `LongListWidget`. The list
emits a contiguous missing logical range and lets the source choose an efficient physical request.
Sources may impose a minimum transport page size; for example a two-item cursor gap can still fetch ten
rows and make the next scroll free.

Random seek remains deliberately bounded. A disconnected thumb jump asks for a small seed around the
target before real geometry is known, rather than turning an estimated screen-wide range into a large
speculative request.

`LongListWidget` deduplicates logical items already requested but not yet reported available. Every
`rangeRequested(first,last,...)` must eventually be paired with
`finishRangeRequest(first,last)`, including failures and differently aligned server responses. A
failure releases suppression but does not immediately reschedule itself, avoiding a tight retry
loop.

Concrete post sources may additionally attach overlapping/adjacent logical waiters to an exact
in-flight edge/cursor request through `PostSourceRequestGate`. That gate does not expand the expected
coverage or create a FIFO prefetch queue; demand that has moved farther away is free to start
independently. Physical-equivalent HTTP coalescing remains in `PostTimelineService`.

The widget must not know whether a result came from an already materialized model object, RAM cache,
SQLite or HTTP.

## Materialization and eviction

The desired viewport window plus buffer defines what must be materialized **now** and which missing
ranges should be requested. It is not the destruction boundary for already-created widgets.

`maxMaterializedItems` is a resident widget budget (initially 200). Already-created widgets are kept
while the total remains within that budget. Consequently a short chat with, for example, 21 posts can
remain fully materialized after the user has visited all of it instead of continually destroying and
recreating rows just outside the current buffer.

When materialization would exceed the budget, `LongListWidget` first protects the current desired
viewport/buffer and evicts the farthest widgets outside that window until the count is back at the
budget. The retained set is therefore allowed to be larger than the current viewport window and does
not have to be one contiguous range.

Evicting a widget means only:

```text
remove child widget from materialized map
retain its measured height
retain source/cache data
```

There is no gap merge and no timeline rebuild. Learned height belongs to the logical item geometry,
not to the lifetime of its current QWidget.

## ChatLogWidget

`ChatLogWidget : LongListWidget` is the first domain-specific layer. It may know about:

- `PostWidget` construction;
- post identity;
- semantic post-ID navigation identity;
- selection/copy;
- context menus;
- edit/highlight operations;
- unread/day decorations;
- post-specific range-loading requests.

It must **not** reimplement scrollbar math, pixel anchoring, viewport-lock timing, user scroll gesture
recognition, materialization, range buffering, seek or item resize handling.

Day separators and the new-messages marker must not become fake logical list items. Prefer either:

1. decoration height associated with a real logical post, or
2. rendering inside the corresponding post widget.

This keeps one list logical index equal to one item in the **view-facing** source.
A model decorator such as `FilteredPostSource` may intentionally map that index to a different raw
`ChannelPostSource` index; geometry still sees only one contiguous logical sequence and never a fake
row for filtered data.

## Semantic navigation and provisional indices

A permalink, Attention item or unread-thread target is identified by **post ID**, not by its current
logical index. This matters because a cached context may know the target post before the source knows
its authoritative server page boundary.

`AbstractPostSource::ensurePostIndex(postId)` may therefore adopt an already cached target into an
estimated empty logical slot when the source has a usable estimated logical coordinate space. This
slot is provisional. When an authoritative server window arrives, the source is allowed to remove
that provisional occurrence and map the same post ID to its real index.

The ownership split is:

```text
post ID / provisional -> authoritative raw index    concrete PostSource
raw index <-> filtered index                         FilteredPostSource (when used)
semantic post ID identity                            ChatLogWidget
view-facing logical item -> persistent viewport      LongListWidget
view-facing index -> pixels / scrollbar / geometry   LongListWidget
```

While semantic navigation is active, `ChatLogWidget` remembers only the target post ID and the last
logical index representing it. If source signals move that ID to another index, `ChatLogWidget` calls
`remapViewportLockedItem(newIndex)`. It does **not** call `scrollToIndex(Center)` again and never
calculates or stores a pixel position.

The existing viewport position therefore survives both source identity remaps and all subsequent
geometry changes. `LongListWidget::viewportLockReleased()` tells `ChatLogWidget` when user intent,
timeout or teardown has ended that semantic navigation state.

Availability follows the same identity rule. `itemsChanged(first,last)` can mean that a provisional
slot became empty even if no QWidget was ever materialized there, so `ChatLogWidget` synchronizes
`LongListWidget` availability bits with `PostSource::isAvailable()` for the whole changed range, not
only for concrete widgets.
