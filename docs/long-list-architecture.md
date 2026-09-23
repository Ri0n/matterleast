# LongListWidget architecture

This document is the implementation contract for long, partially materialized widget lists in
Mattermost-Qt. The first consumer is the chat log, but `LongListWidget` itself is deliberately not
message-aware.

The previous sparse timeline implementation is retained under `deprecated/` only as migration
reference. New code must not include or link it.

This is the entry point for the virtualized long-list subsystem. The detailed contract is split by responsibility so viewport work does not require loading post-source or cache details.

## Detailed documents

- [Core ownership and layering](long-list/core-and-layering.md) — core rule, class layering, widget responsibilities and the no-gap invariant
- [Viewport geometry and scrolling](long-list/viewport-and-scrolling.md) — geometry transactions, anchors, persistent viewport locks, wheel/scrollbar behavior and random seeks
- [Loading and materialization](long-list/loading-and-materialization.md) — demand policy, widget materialization/eviction, ChatLogWidget and semantic navigation
- [Sources, cache boundary and migration](long-list/sources-and-boundaries.md) — post-source integration, logical counts, cache boundary, ownership prohibitions, migration notes and required tests

## Context-loading rule

Start with this page. Open only the detailed document that matches the code or invariant being changed. Do not load the whole subsystem documentation unless the change genuinely crosses those boundaries.
