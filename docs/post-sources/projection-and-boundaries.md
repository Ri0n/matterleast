# Post sources: projection and boundary requests

> Part of [Post source architecture](../post-source-architecture.md). Read the landing page first and load this file only when this area is relevant.

## `FilteredPostSource`: predicate projection

`FilteredPostSource` is a reusable model-layer decorator:

```cpp
using Predicate = std::function<bool(const BackendPost&)>;

FilteredPostSource(AbstractPostSource& source, Predicate predicate, QObject* parent = nullptr);
```

A predicate returning `true` keeps the post in the projected sequence; `false` removes it from that
sequence entirely. There is no zero-height or one-pixel view row. The view sees a real contiguous
logical model containing only accepted posts.

The key invariant is that filtering creates a **projection**, not a new transport authority:

```text
server / cache
      |
      v
ChannelPostSource                 raw coordinate system
  [A B C D E F]
      |
      | predicate rejects B,E
      v
FilteredPostSource                view coordinate system
  [A C D F]
      |
      v
ChatLogWidget / LongListWidget
```

The decorator is responsible for:

- filtered-index -> wrapped-source-index translation for `requestRange()`;
- wrapped-source -> filtered-index translation for availability, seek and layout signals;
- structural `itemsInserted` / `itemsRemoved` when predicate decisions change visible topology;
- preserving semantic identity through `postIdAt()`;
- caching rejected post IDs across resident-body eviction;
- leaving unresolved/non-resident rows provisionally present until there is enough post data to
  evaluate the predicate;
- `setPredicate()` / `invalidatePost()` for future policies whose acceptance can change.

It must **not**:

- modify `ChannelPostSource::postIds`;
- change `total_msg_count_root` reconciliation or absolute-page placement;
- reinterpret thread cursor/count semantics;
- invent placeholder rows to preserve old indices;
- perform pixel/viewport work.

The projection deliberately has its own indices. When a newly resolved post becomes rejected, the
decorator emits a structural removal in filtered coordinates. `LongListWidget` then shifts its
logical geometry through the normal `removeItems()` transaction. A semantic viewport lock survives
because the target widget identity is remapped to its new filtered index; its screen Y is preserved.

Navigation that must seed raw Mattermost context first (for example
`ChannelPostSource::adoptNavigationContext()`) unwraps the decorator and talks to the raw channel
source. The resulting source mutations flow back through the projection before
`ChatLogWidget` establishes its semantic post-ID lock. This keeps permalink placement and filtering
as separate responsibilities.

The current main-channel policy uses this decorator to hide routine membership churn. Thread timelines
currently consume `ThreadPostSource` directly.

## Shared boundary-request attachment

Ordinary scrolling can produce another range demand while an exact edge/cursor request is still in
flight. Starting an equivalent HTTP request wastes bandwidth, but queuing arbitrary later demands
behind the first request is also wrong when the user scrolls quickly.

`PostSourceRequestGate` therefore tracks only the logical range an exact physical request is expected
to cover plus the logical request waiters attached to it:

```text
in-flight expected range: 130..149
new demand 120..129        -> attach (adjacent)
new demand 135..145        -> attach (overlap)
new demand 100..119        -> do not attach; may start independently
```

Attaching a waiter does **not** expand the expected range. When the HTTP request completes, the source
finishes all attached logical requests and lets `LongListWidget` recompute demand from the current
viewport. There is no source-side FIFO chain that blindly continues loading stale ranges after the user
has moved elsewhere.

Seek requests are not attached to ordinary boundary work: a new random seek has its own generation and
must be free to supersede old viewport intent.

This source-level gate is distinct from `PostRepository` request coalescing. The gate reasons about
logical coverage and current viewport demand; repository coalescing only deduplicates physically
equivalent HTTP requests.
