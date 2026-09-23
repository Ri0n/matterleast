# Post source architecture

This document defines the ownership boundary shared by channel and thread timelines. It complements
`long-list-architecture.md` (viewport/geometry ownership) and `post-cache.md` (resident/durable post
storage). The central rule is that a source owns **logical post identity and transport planning**, never
pixels.

This is the routing page for timeline presentation sources. Load only the source layer you are changing; channel/thread and projection contracts deliberately remain separate.

## Detailed documents

- [Interface and indexing](post-sources/interface-and-indexing.md) — layering, AbstractPostSource and IndexedPostSource identity/mutation rules
- [Projection and boundary requests](post-sources/projection-and-boundaries.md) — FilteredPostSource and shared boundary-request attachment
- [Channel and thread sources](post-sources/channel-and-thread.md) — channel topology, thread topology and live-reply transactions
- [Presentation chain and local outgoing augmentation](post-sources/presentation-chain.md) — generic projection/augmentation stack, optimistic outbox, retry/correlation/recovery and viewport invariants
- [Extension rules](post-sources/extension-rules.md) — what must not be shared, cache interaction and rules for adding source layers

## Context-loading rule

Start with this page. Open only the detailed document that matches the code or invariant being changed. Do not load the whole subsystem documentation unless the change genuinely crosses those boundaries.
