# Emoji resolution and picker search

MatterLeast has one shared emoji registry in `EmojiInfo`. Built-in emoji are static; custom emoji become registry entries only after their image is available locally.

## Custom emoji sources

Custom emoji can enter the registry through three paths:

1. The legacy startup `GET /emoji` loader provides an initial server sample.
2. Message rendering can encounter an unknown `:name:`; `CustomEmojiService` resolves that name lazily through the batch/per-name API and caches its image.
3. The emoji picker searches the server catalog with `POST /emoji/search`. Search results are passed through `CustomEmojiService` and the same image cache before they are registered.

Do not treat the startup custom-emoji list or the picker's current tabs as an exhaustive server catalog.

## Synchronization

`EmojiRegistryNotifier::customEmojiAdded` is the synchronization point after a custom emoji becomes usable. Consumers that cache presentation/search state must refresh from `EmojiInfo` when this signal arrives.

The picker therefore keeps built-in/local filtering immediate, performs server custom-emoji search after its existing debounce, and refreshes both its search index and Custom tab when matching images finish resolving. Network/download/cache ownership stays in `CustomEmojiService`; UI code must not create a second custom-emoji cache.
