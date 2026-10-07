# Emoji resolution and picker search

MatterLeast deliberately separates semantic emoji identity from presentation
storage.

Mattermost `emoji_name` is the application/wire identity. Generated built-ins
are immutable process-wide data in `EmojiInfo`; runtime custom emoji are
backend-scoped cache entries in `EmojiRegistry`. There is no numeric runtime
emoji identity.

## Lazy custom emoji sources

MatterLeast does not enumerate or eagerly download the server custom-emoji
catalog at startup. A custom emoji is resolved only when there is a concrete
reason to need it:

1. **Reaction ranking prewarm.** After authenticated login, the small persisted
   reaction ranking is resolved by name. Built-ins stay local; unknown custom
   names use the lazy resolver.
2. **Message/reaction rendering.** An unknown valid `:name:` resolved through
   the backend's `EmojiRegistry` emits `customEmojiRequested`.
   `CustomEmojiService` resolves it through the batch/per-name Mattermost API
   and caches its image.
3. **Picker search.** The picker filters already-known emoji immediately and
   also calls `POST /emoji/search` after its debounce. Matching custom emoji
   use the same backend-scoped registry and disk cache.
4. **Explicit Custom-tab browsing.** Opening the Custom category loads only the
   first browse page on demand.

The picker cache is a working set, not an exhaustive copy of the server's custom
emoji catalog.

## Ownership and synchronization

Each `Backend` owns one `EmojiRegistry`, and `CustomEmojiService` is a direct
child of the same backend. A registry miss emits
`EmojiRegistry::customEmojiRequested` only to that backend's resolver.
Completion emits `customEmojiAdded` only to UI bound to the same backend.

Consumers that retain presentation/search state refresh on
`EmojiRegistry::customEmojiAdded`. The picker updates its searchable/custom
views and post widgets re-render matching unresolved reaction names.

Resetting a backend clears runtime registry and resolver state. Generated
built-ins remain immutable and shared.

Reaction identity is never owned by presentation storage. `BackendPost` keeps
the exact Mattermost `emoji_name` received from REST/WebSocket state, including
aliases and unresolved custom names. Presentation lookup cannot rewrite the
name later used by tooltips, add/remove actions, or reaction events.

## Registry storage and resource bounds

Generated built-ins use an immutable hash lookup from name/alias to compact
presentation coordinates (`kind + category + index`) in generated category
vectors. Categories own their emoji; no category is reconstructed from a global
sequence number.

Runtime custom emoji never enter generated maps or vectors. `EmojiRegistry`
stores only a bounded LRU working set of up to 1024 `name -> cached image path`
entries per backend. Presentation HTML is constructed on demand, so the registry
does not retain one HTML string or decoded image per custom emoji.

Eviction is safe because the Mattermost name remains the identity. A later
lookup of an evicted name simply becomes a lazy resolver miss and can be
restored from the server metadata/local disk cache. No stale numeric handle can
point at a different emoji.

Custom-image classification is intentionally independent of the LRU entry
itself. The registry records the dedicated custom-emoji cache directories, so a
QTextDocument that already contains a custom `<img>` remains recognizable even
if that name's metadata entry has since been evicted.

The persistent image cache is server-scoped. Cached files live below a hash of
the normalized server identity under the application `custom-emoji/` cache, so
identical Mattermost emoji IDs from different servers cannot alias the same
file. The complete disk cache is capped at 128 MiB and prunes older files by
last-use/download timestamp.

A `QPixmap` or icon may exist while a concrete reaction chip, quick-bar button
or picker button is on screen, but neither startup nor the registry decodes the
whole custom catalog into RAM.

## Original failure and invariants

The old design gave generated built-ins and runtime custom emoji one numeric
`EmojiSeq` namespace. Runtime registration extended the generated custom range
until it overlapped the skin-variadic range. A correct name such as `+1` /
`thumbsup` could therefore keep the correct tooltip/action identity while
rendering an unrelated custom image; restart temporarily hid the problem by
rebuilding a smaller registry.

The current design makes that class of corruption structurally impossible.
Future changes must preserve these invariants:

- wire-level/application identity is always the Mattermost emoji name;
- generated lookup coordinates describe immutable built-in storage only;
- runtime custom emoji are backend-scoped, bounded, and resolved by name;
- loading runtime custom emoji never mutates generated maps or vectors;
- aliases may share generated storage coordinates but are not persistent
  application identity;
- no process-global runtime custom registry or notification bus is introduced.

## Picker theme propagation

The emoji picker must follow live application palette changes without rebuilding
all category pages.

Do not use a stylesheet on the picker navigation just to control geometry.
A stylesheet can materialize palette state for already-created picker children,
leaving them in the old light/dark colors after `QApplication` changes
palette. The custom category buttons read their current palette while painting.

The picker root uses `QPalette::Base` as its background role rather than
`QPalette::Window`. This intentionally tracks the theme's content surface
(typically white in light themes and black/dark in dark themes) while still
following live palette changes.

The picker no longer uses native `QTabBar` chrome for category navigation.
Native tab styles vary too much across platforms (selected-tab shifts, base
lines, large implicit padding and icon/text alignment). Instead it uses a small
row of fixed-size, palette-aware category buttons above a `QStackedWidget`.
The buttons paint only a subtle selected/hover background and the centered emoji
or custom-category icon, so active and inactive tabs have identical geometry and
there is no separate tab-bar base line.

The emoji body uses a real flow layout. Emoji buttons keep their fixed
interaction size while the layout wraps according to the current page width.
This gives the page a small minimum width and makes resizing naturally reflow
the contents instead of keeping a hidden fixed-width grid or manually moving
widgets from a resize handler.

Each category page lives inside a `QScrollArea`, so reducing the dialog height
keeps the category usable with the mouse wheel and thumb. The application-wide
`OverlayScrollBarManager` supplies the thin overlay scrollbar, but the picker
opts out of the top/bottom edge-jump buttons because those affordances are chat
navigation rather than picker navigation. The flow host updates its minimum
height from `heightForWidth()` so the scroll range follows wrapping as the
dialog width changes.

This is the same general rule used elsewhere in MatterLeast for live theme
propagation: avoid per-widget stylesheets when a palette/geometry API can express
the same behavior.

## Server search semantics

The picker mirrors the official Mattermost behavior by querying:

```text
POST /api/v4/emoji/search
{"term":"..."}
```

The server term removes surrounding shortcode colons but otherwise preserves custom-name punctuation. In particular, `foo-bar` and `foo_bar` are not collapsed into the same server query.

Built-in/local matching can still normalize spaces and hyphens for convenient client-side search; that normalization must not leak into the server custom-emoji term.

## Memory/network policy

Startup work should scale with the user's small working set, not with the number of custom emoji installed on the server.

Therefore:

- reaction-ranking names may be prewarmed;
- referenced/searched custom emoji may be downloaded;
- opening the Custom picker category may load its bounded first browse page;
- the catalog is not downloaded just because the user logged in;
- adding more custom emoji on the server must not linearly increase MatterLeast startup memory or network traffic.
