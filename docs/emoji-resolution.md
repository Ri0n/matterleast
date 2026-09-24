# Emoji resolution and picker search

MatterLeast has one shared emoji registry in `EmojiInfo`. Built-in emoji are static. Custom emoji become registry entries only after their image is available in the local disk cache.

The registry stores names and presentation paths, not a permanent in-memory copy of every server image.

## Lazy custom emoji sources

MatterLeast does not enumerate or eagerly download the server custom-emoji catalog at startup.

A custom emoji is resolved only when there is a concrete reason to need it:

1. **Reaction ranking prewarm.** After authenticated login, the small persisted reaction ranking is resolved by name. Built-ins stay local; unknown custom names use the lazy resolver.
2. **Message/reaction rendering.** If parsing encounters an unknown valid `:name:`, `EmojiInfo::findByName()` emits `customEmojiRequested`. `CustomEmojiService` resolves the name through the batch/per-name Mattermost API and caches its image.
3. **Picker search.** The emoji picker filters already-known emoji immediately and also calls `POST /emoji/search` after its debounce. Matching custom emoji are routed through `CustomEmojiService` and the same disk cache before they enter `EmojiInfo`.
4. **Explicit Custom-tab browsing.** Opening the Custom category loads only the first browse page on demand. This preserves ordinary browsing without turning login into a catalog preload.

Do not treat the picker's current Custom tab, the local registry, or any previous search result as an exhaustive server catalog.

## Cache ownership

`CustomEmojiService` owns custom-image resolution and disk caching. UI code must not create a second custom-emoji cache.

The cache lives under the application cache directory in `custom-emoji/`. Registry entries point at those cached files.

A `QPixmap`/icon may of course exist while a concrete reaction chip, quick-bar button or picker button is on screen, but there is no startup policy that loads the entire custom catalog into RAM.

## Synchronization

`EmojiRegistryNotifier::customEmojiAdded` is emitted after a custom emoji image has become usable and `EmojiInfo` has registered the name.

Consumers that cache presentation/search state must refresh from `EmojiInfo` on this signal.

The picker uses it to refresh:

- its local searchable set;
- the Custom tab if the newly registered emoji changes that tab;
- the active search result view.

Post widgets use the same signal to re-render unresolved custom emoji and to promote unresolved named reactions into normal reaction chips.

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
