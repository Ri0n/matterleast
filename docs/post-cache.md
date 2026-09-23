# Post cache architecture

This document defines the persistent and resident post-cache contract below the chat post sources.
It extends the cache boundary in `long-list-architecture.md`; it does not move any geometry or
viewport responsibility out of `LongListWidget`.

`post-cache-runtime.md` is the detailed execution contract for snapshot authority, account isolation,
observation sequences, async worker ordering, direct cache-first reads and future provisional newest
window hydration. `post-source-architecture.md` defines which logical-index operations are shared by
channel/thread sources and which boundary proofs stay transport-specific.

This page describes the architectural boundaries of the post cache. Runtime sequencing and paging details live behind the separate runtime landing page.

## Detailed documents

- [Admission and persistence](post-cache/admission-and-persistence.md) — goals, admission policy, durable storage, worker-thread ownership, causal writes and disk compaction
- [Authority and resident memory](post-cache/authority-and-memory.md) — authority rules, invalidation, resident-memory policy and settings
- [Rollout and reconnect validation](post-cache/rollout-and-reconnect.md) — implementation phases and reconnect/timeline validation
- [Runtime contract](post-cache-runtime.md) — snapshot causality, cache-first loading, paging and failure behavior

## Context-loading rule

Start with this page. Open only the detailed document that matches the code or invariant being changed. Do not load the whole subsystem documentation unless the change genuinely crosses those boundaries.
