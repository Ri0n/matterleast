# Long list: viewport geometry and scrolling

> Part of [LongListWidget architecture](../long-list-architecture.md). Read the landing page first and load this file only when this area is relevant.

## Geometry ownership and transaction

Any operation that can change effective item heights uses one synchronous transaction owned by
`LongListWidget`:

```text
capture ordinary anchor / inspect persistent viewport lock
        |
block scrollbar signals
freeze viewport updates
        |
create / destroy widgets
measure dirty sizeHints
update height index
recompute scrollbar range
        |
restore seek target, persistent lock, or ordinary anchor against NEW geometry
position materialized widgets
        |
unfreeze viewport
unblock signals
paint once
```

There is no second pixel-anchor restore in a base class or controller and no queued paint-resume
owner.

Child widgets are installed on the viewport and watched for `LayoutRequest` / `Resize`. Multiple
changes in one event-loop turn are coalesced. The next geometry transaction measures all dirty
widgets together before the viewport is allowed to move or repaint.

Those events are only **hints**. If remeasurement proves that every dirty row kept the same logical
height, the transaction is a no-op: it must not restore anchors, relayout the list, emit range
changes, or wake `rangeRequested`. This is important for hover/reaction affordances and other
child-widget activity: moving the mouse over posts must never turn an unresolved neighbouring gap
into transport polling.

Structural HeightIndex changes are different. Changing logical item count or the default estimate
changes content extent even when there is no dirty materialized QWidget. Such operations must force
scrollbar-range/anchor reconciliation; they must never depend on a dirty-row measurement to make the
new logical extent visible to `scrollToEnd()`, sparse seek, or viewport demand.

A semantic child signal that explicitly guarantees finalized size hints (for example
`PostWidget::dimensionsChanged`) uses `commitItemGeometryNow()` instead of the deferred hint path.
That transaction updates the HeightIndex and physical QWidget atomically before another scroll/sync
can reapply an obsolete estimated height. Generic Qt `LayoutRequest` / `Resize` events must not use
this synchronous path because they may be presentation-only noise.

Likewise, ordinary `insertItems()`/viewport synchronization must not globally disable and re-enable
viewport updates merely to protect a synchronous geometry sequence. Those operations do not re-enter
the event loop, so no intermediate paint can occur; re-enabling QWidget updates would instead
invalidate the entire viewport and repaint every materialized post. Child show/move/hide operations
are sufficient to invalidate only the affected regions.

A same-index semantic replacement (for example optimistic pending -> authoritative server post) uses
`replaceItem(index)`. The replacement is created and measured while the old row is still visible.
A replacement whose first measurement differs from the resident row is first kept hidden for one
event-loop turn and remeasured after queued child-layout work settles. The resident row remains
visible during that preparation. This prevents transient construction sizes (for example wrapped
rich text before its width-dependent height settles) from becoming user-visible geometry. If the
settled height is unchanged (the normal optimistic-promotion case), the two child widgets are then
swapped in the same rect and only that rect is invalidated; the viewport is **not** globally
disabled/re-enabled, because re-enabling QWidget updates itself schedules a full repaint.
Only a real height/availability change enters the scrollbar/anchor transaction that may move other
rows. Even that transaction must not globally disable/re-enable viewport updates: it is synchronous
and does not re-enter the event loop, while re-enabling QWidget updates invalidates every materialized
child and can make expensive PostWidgets visibly flash. Unrelated widgets are retained. Do not model
replacement as `layoutChanged`, nor as a visible `itemsInserted` followed by `itemsRemoved`.

A delayed image, Markdown reflow, reaction row or thread button therefore cannot independently move
the chat viewport.

## Ordinary anchor semantics

For ordinary reading, the anchor is the logical item intersecting the viewport top plus the offset
inside that item. If the viewport is explicitly attached to the newest edge, the anchor is `Bottom`
instead.

Consequences:

- height changes entirely above the viewport preserve the visible content at exactly the same screen
  coordinates;
- height changes while sticky-bottom is active preserve the real end;
- normal appended logical items preserve sticky-bottom intent;
- an explicit `InsertViewportPolicy::PreserveVisibleContent` append converts a current Bottom anchor
  to the old concrete item anchor, allowing the new tail to settle just below the viewport until a
  separate semantic follow operation reveals it;
- prepending real logical items shifts logical indices without moving the already visible content;
- a window resize cannot leave an unreachable last few pixels;
- pruning/materialization cannot reinterpret a pixel offset as a different logical item.

A geometry change *inside* the visible region cannot keep every row on both sides at the same Y: if a
visible item grows by 200 pixels, some content must make room for those pixels. The invariant is that
`LongListWidget` preserves its chosen semantic anchor and introduces no additional artificial jump.

Only direct user input or an explicit logical navigation operation changes viewport intent. Sticky
bottom is exact user intent: only the actual scrollbar maximum captures a `Bottom` anchor. A thumb
position even one pixel above the maximum is an ordinary item anchor and must not be snapped back to
the end by a later materialization or geometry transaction.

## Persistent viewport lock semantics

Explicit semantic navigation needs a stronger invariant than the ordinary top-of-viewport anchor.
For example, a permalink target may be placed near the middle of the viewport and then experience
attachment loading, Markdown reflow, logical prepends or a provisional-to-authoritative index remap.

`LongListWidget::lockViewportToItem()` applies the requested `Alignment` **once** and then records the
target item's **top edge as a fraction of the viewport height**:

```text
itemTopFraction = itemTopViewportY / viewportHeight
```

After that, alignment is no longer re-applied. Geometry transactions restore the item top from the
stored fraction:

```text
same viewport height:
    500 px / 1000 px -> 500 px / 1000 px

viewport resized to half height:
    500 px / 1000 px -> 250 px / 500 px
```

This gives two desired properties at once:

- ordinary reflow/loading with unchanged viewport height keeps the locked item at the same absolute
  screen Y;
- resizing the viewport keeps the locked item at the same relative vertical position.

The lock is expressed only in logical-item coordinates at the public boundary. All conversion to
content pixels and scrollbar values remains private to `LongListWidget`.

`remapViewportLockedItem(newIndex)` changes only the logical index represented by the lock. The stored
screen position is preserved; the item is **not** centered again. Logical insertions before the lock
automatically shift its index as part of the same structural transaction.

A direct wheel, scrollbar action or thumb drag immediately gives authority back to the user and
releases the persistent lock. A positive quiet period may also release it; zero means that only user
intent or explicit teardown releases it.

## Wheel and scrollbar scrolling

Wheel scrolling is ordinary pixel movement, but that fact is private to `LongListWidget`.
Recognition of wheel, scrollbar action and thumb-drag intent belongs entirely to the list; domain
subclasses do not inspect scrollbar values.

Each pixel movement performs only the work that belongs to pixel geometry first:

```text
scrollbar value changes
  -> reposition already-materialized widgets
  -> translate geometry into logical item state
  -> emit visibleRangeChanged only if the logical visible range changed
  -> emit itemVisibilityChanged only if Body/Top/Bottom changed
  -> synchronize/materialize only if the desired logical demand range changed
```

This makes ordinary scrolling **boundary-driven rather than pixel-driven**. A 1 px move inside the
same logical demand window does not re-run materialization and does not re-request an unresolved
neighbouring range. A 200 px move inside one oversized item may likewise require no item-level sync.
Conversely, a small move that crosses an item/prefetch boundary does.

Visibility changes carry `UserScroll`, `ProgrammaticScroll` or `LayoutChange`. Direct user input
also emits `userScrollStarted()` as viewport-ownership intent even when no visibility mask changes.

When the desired logical range actually changes, `LongListWidget` reports each contiguous missing
run as logical demand; it does not split that run into arbitrary transport-sized ten-item blocks.

This distinction is important: a desired tail such as `131..161` should reach the source as one range.
The source may then satisfy it with one exact newest-edge request rather than treating `130..139`,
`140..149`, `150..159`, and `160..161` as independent transport jobs. The widget describes **what it
needs**; the source decides **how to fetch it**.

Loading adjacent data never recenters the viewport.

## Programmatic tail reveal

`scrollToEndAnimated()` is a semantic convenience implemented entirely inside
`LongListWidget`. Callers request "reveal the end"; they do not supply pixel
coordinates.

The intended optimistic-send sequence is:

```text
append local tail with PreserveVisibleContent
    -> old concrete viewport stays fixed
    -> new row may materialize and settle below the viewport
    -> scrollToEndAnimated()
    -> item visibility/range boundaries update during the motion
    -> final EnsureVisible synchronization at the real current end
```

The animation is short and only used when the end is within one viewport of the
current position. A farther destination falls back to the ordinary immediate
`scrollToEnd()`; animating through unrelated history would be disorienting.

The animation interpolates an internal scrollbar value, but every frame still
passes through the same item-semantic boundary:

- existing widgets move physically;
- `ItemVisibility` changes use `ProgrammaticScroll`;
- buffered logical synchronization occurs only when the desired item range
  actually changes;
- any direct user wheel/scrollbar gesture stops the animation immediately and
  transfers viewport ownership to the user.

If content height changes while the animation is running, the animation targets
the current scrollbar maximum rather than a stale pixel endpoint. Once the end
is reached, normal Bottom-anchor semantics own later geometry changes.

## Random thumb seek

A scrollbar value represents the **top content offset of a viewport**, not a post ordinal. Thumb
movement therefore first maps the scrollbar value back into the current estimated content geometry
and chooses the logical item at the viewport centre:

```text
top    = contentOffsetForScrollValue(value)
center = top + viewportHeight / 2
target = heightIndex.indexAtPixel(center)
```

This distinction matters near the newest edge: moving the thumb upward by one pixel must not still
select the final post and then re-centre it, which would snap the scrollbar back to the end.

Thumb dragging has two modes:

```text
target already materialized OR desired viewport bodies already available
        -> ordinary buffered scroll path immediately

jump enters unavailable / unmaterialized history
        -> seek target + 100 ms debounce
```

The first mode intentionally behaves like wheel scrolling: it keeps the user's exact scrollbar
position and materializes the viewport/buffer immediately. No seek re-centering is allowed merely
because the user happened to drag the thumb instead of using the wheel.

Only a genuine random jump into data that is not ready enters seek mode. While such a thumb target is
moving, target changes restart the 100 ms debounce timer. When it becomes stable:

```text
request ~10-item seed around TARGET
        |
materialize + measure seed
        |
center TARGET using the new geometry
        |
calculate actual viewport + buffer coverage
        |
request additional contiguous missing demand
```

A new seek target increments the seek generation. Results from an older generation may still enter
the memory/disk cache, but `LongListWidget` only materializes what the current viewport/seek needs,
so stale results have no authority to move the viewport.

For channel history the newest edge and known post identities are preferred over absolute-page
arithmetic during ordinary reading. A demand touching the known newest edge is bootstrapped with one
`page=0` request sized for that demand; after that, adjacent gaps are filled through `before`/`after`
post identities. Absolute pages remain useful for genuinely disconnected random positioning and for
the oldest-boundary repair described below. This prevents an approximate channel row count from
influencing every wheel-scroll request after exact identities already exist.

`total_msg_count_root` is only an initial coordinate estimate for `/posts`, not its row count.
Deleted roots can make the counter larger than visible history, while join/leave and other system
roots excluded from Mattermost message counts are still returned by `/posts` and can make the counter
smaller. The source therefore repairs the oldest boundary in both directions. Absolute pages remain
newest-anchored; count growth inserts empty logical slots at the oldest side so already mapped pages do
not move relative to the newest edge.

Boundary search stays in ten-post page coordinates but tests distant candidate page starts with one
root only: candidate page P is probed as `page=P*10&per_page=1`. For a large top-edge request the first
probe jumps inward by a heuristic 3% of the estimated root count. If it is empty, the step grows
exponentially farther inward until data is found. If it exists, binary search walks outward to the
reported boundary. A full reported-boundary page is not accepted as proof: the source first probes the
adjacent older page and, if that exists too, expands outward exponentially until `/posts` provides an
empty/short boundary. Small estimates use the normal ten-post path first and enter the same outward
repair only when their reported oldest page is full.

Near a bounded edge, when at most two unknown ten-post pages remain, the source stops spending
one-root probes and materializes the first unknown page with `per_page=10`. A short page proves the
exact count; a full page adjacent to known emptiness proves an exact multiple of ten. Exact
reconciliation may therefore remove a phantom prefix or insert a missing oldest prefix. The 3% value
changes latency only, never correctness.

This replaces the old controller-level `TimelineSeekState` state machine.
