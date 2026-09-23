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
- appending a logical item while sticky-bottom is active keeps the viewport on the new end;
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

Wheel scrolling is ordinary pixel movement. Recognition of wheel, scrollbar action and thumb-drag
user intent belongs entirely to `LongListWidget`; domain subclasses do not override wheel handling or
inspect scrollbar signals.

After the scrollbar value changes, `LongListWidget` computes the visible logical range plus a
configurable buffer. For ordinary scrolling it reports each contiguous missing run as logical demand;
it does not split that run into arbitrary transport-sized ten-item blocks.

```text
user scroll gesture
  -> release persistent viewport lock
  -> change scrollbar value
  -> compute logical visible + buffer range
  -> materialize available items
  -> request contiguous unavailable demand
  -> emit userViewportChanged(atEnd)
```

This distinction is important: a desired tail such as `131..161` should reach the source as one range.
The source may then satisfy it with one exact newest-edge request rather than treating `130..139`,
`140..149`, `150..159`, and `160..161` as independent transport jobs. The widget describes **what it
needs**; the source decides **how to fetch it**.

Loading adjacent data never recenters the viewport.

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
