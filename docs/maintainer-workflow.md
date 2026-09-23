# Maintainer workflow

This document contains repository-wide workflow rules that are useful to contributors and coding agents but are not architecture for any single subsystem.

## Pull requests and commits

- Keep ordinary PRs to one meaningful commit. During development, amend or squash rather than leaving fixup commits in the final PR.
- Use a descriptive commit message that summarizes the implemented behavior and important rationale.
- Do not merge code changes before the required CI jobs are green.
- If a change has meaningful manual-regression risk, keep it available for manual testing after CI rather than treating compilation alone as sufficient validation.

## Documentation

- Architectural decisions and hard-to-rediscover invariants belong in the repository, not only in chat history.
- Start from [README.md](README.md) and follow only the documentation path relevant to the task.
- Keep landing pages short and put details in focused child documents.
- Avoid duplicating the same rule across several files; link to the authoritative location instead.
- Move obsolete design material to `deprecated/` so old and current contracts cannot be confused.

## Scope discipline

Prefer changes at the layer that owns the behavior. In particular, presentation-only behavior should normally be modeled in presentation/source layers rather than by corrupting authoritative backend state or hiding rows with layout hacks.

Keep public API surface small. Helpers and parsing/implementation details that do not need to be public should remain private to their implementation.
