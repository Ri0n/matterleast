# Following, Attention and read tracking

This document is the normative contract for read progress used by the Following
and Attention sidebar views. The central rule is deliberately simple:

> A post becomes read when the lower edge of its concrete post widget enters the
> active viewport.

Navigation is not read acknowledgement. A click, successful permalink lookup,
selection change, post materialization, or navigation highlight may cause the
viewport to be re-evaluated, but none of those events by itself marks anything
read.

This rule applies to normal channel timelines and thread timelines alike.

Read tracking is cross-cutting, so the top-level contract remains here while mechanics are split into scrolling/resume versus projection/server-acknowledgement behavior.

## Detailed documents

- [Following behavior](following.md) — normative queue membership, sorting, activation and repeated-click behavior
- [Scrolling and resume cursor](read-tracking/scroll-and-resume.md) — ownership, the single read rule, sticky-bottom live tail and monotonic resume cursor
- [Projections and server acknowledgement](read-tracking/projections-and-acknowledgement.md) — Following/Attention projection rules, thread/channel acknowledgement, re-evaluation events and invariants

## Context-loading rule

Start with this page. Open only the detailed document that matches the code or invariant being changed. Do not load the whole subsystem documentation unless the change genuinely crosses those boundaries.
