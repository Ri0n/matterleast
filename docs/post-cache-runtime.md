# Post cache runtime contract

This document describes the runtime behavior of the Mattermost post cache: ownership, snapshot
authority, asynchronous ordering, account isolation, read/write flows and the boundary between cached
data and logical timeline placement. `post-cache.md` contains storage policy and limits;
`post-source-architecture.md` defines source-side logical paging. This file is the detailed runtime
execution contract between those layers.

Use this page when debugging runtime cache behavior. The contract is split into snapshot causality, loading/paging and policy/testing so a paging problem does not require loading persistence and memory-policy material.

## Detailed documents

- [Snapshots and causality](post-cache/runtime-snapshots-and-causality.md) — ownership, snapshot authority, stable resident refresh, observation/write ordering and account isolation
- [Loading and paging](post-cache/runtime-loading-and-paging.md) — async cache service, direct loadPost, hydration, channel/thread paging and request coalescing
- [Runtime policy and tests](post-cache/runtime-policy-and-tests.md) — store/memory policy, failures, invalidation matrix, non-ownership rules, implementation status and regression requirements

## Context-loading rule

Start with this page. Open only the detailed document that matches the code or invariant being changed. Do not load the whole subsystem documentation unless the change genuinely crosses those boundaries.
