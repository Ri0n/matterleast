# MatterLeast agent bootstrap

This file is the entry point for automated coding agents working in this repository.

## Before changing code

1. Read [docs/README.md](docs/README.md).
2. From that index, open only the subsystem landing page relevant to the task.
3. Load only the detailed documents required for the code or invariant being changed.
4. Do not preload the whole `docs/` tree. The documentation is intentionally structured for progressive disclosure.

## Engineering rules

- Preserve existing architectural ownership and invariants unless the task explicitly changes them.
- Prefer model/source-layer solutions over UI-only workarounds when the behavior belongs to presentation or data flow.
- Treat `AbstractPostSource::layoutChanged` as a last-resort structural signal. It is allowed only for a real semantic identity-to-index remap that cannot be expressed as exact insert/remove/availability/replacement events. Never use it for content updates, pending/delivery state, ordinary geometry changes, append/remove, or optimistic confirmation. Read the structural-signal rules in `docs/post-sources/interface-and-indexing.md` before adding a new producer.
- When a change introduces a non-obvious invariant, ownership rule, restart/failure semantic, CI/platform trap, or deliberate limitation, update the appropriate documentation in the same PR.
- Keep public headers minimal; implementation-only helpers belong in private implementation where practical.
- Ordinary PRs should contain one meaningful commit. Amend/squash before merge rather than accumulating fixup commits.
- Do not merge code changes until required CI is green.
- Keep commit messages descriptive enough to explain the implemented behavior, not merely the issue number or a generic "fix".

## Documentation routing

The authoritative documentation index is [docs/README.md](docs/README.md). Top-level files in `docs/` are landing pages; detailed subsystem contracts live in focused subdirectories. Historical/obsolete architecture belongs under `docs/deprecated/`.
