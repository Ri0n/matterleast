# Maintainer workflow

This document contains repository-wide workflow rules that are useful to contributors and coding agents but are not architecture for any single subsystem.

## Pull requests and commits

- Keep ordinary PRs to one meaningful commit. During development, amend or squash rather than leaving fixup commits in the final PR.
- Use a descriptive commit message that summarizes the implemented behavior and important rationale.
- Do not merge code changes before the required CI jobs are green.
- If a change has meaningful manual-regression risk, keep it available for manual testing after CI rather than treating compilation alone as sufficient validation.

## CI and packaging

Commit/PR CI is intentionally a fast compatibility gate, not a packaging pipeline. It runs exactly these build-and-test targets:

- Ubuntu 22.04 with the distribution Qt 5.15.3 packages.
- Ubuntu 24.04 with the distribution Qt 6.4.2 packages.
- Windows 2025 with Qt 6.10.3.

Do not add installer, DEB, RPM or artifact-production work to the commit CI. Packaging lives in `.github/workflows/packages.yml`, which is invoked manually or by the tag-driven release workflow.

The packaging workflow owns release artifacts for Ubuntu 22.04, Ubuntu 24.04, Ubuntu 26.04, Fedora 44 and Windows. Ubuntu 24.04 is packaged only with Qt 6. The release workflow must verify the complete expected artifact set before publishing a GitHub release.

## Documentation

- Architectural decisions and hard-to-rediscover invariants belong in the repository, not only in chat history.
- Start from [README.md](README.md) and follow only the documentation path relevant to the task.
- Keep landing pages short and put details in focused child documents.
- Avoid duplicating the same rule across several files; link to the authoritative location instead.
- Move obsolete design material to `deprecated/` so old and current contracts cannot be confused.

## Scope discipline

Prefer changes at the layer that owns the behavior. In particular, presentation-only behavior should normally be modeled in presentation/source layers rather than by corrupting authoritative backend state or hiding rows with layout hacks.

Keep public API surface small. Helpers and parsing/implementation details that do not need to be public should remain private to their implementation.
