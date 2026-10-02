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
- Working feature/bugfix branches and open PRs may contain as many development commits as useful. PR commit count is not a cleanup target: do not squash, rebase, rewrite, or force-push a branch merely to make the PR contain one commit.
- Put `[skip ci]` in intermediate commit messages when that commit is not intended to be built/tested on its own. Leave it out of commits whose resulting branch state should run CI.
- Merge ordinary code changes to `master` with GitHub squash merge. The requirement for one meaningful commit applies only to the resulting squash-merge commit on `master`, not to the commits inside the PR branch. Its title/body must describe the implemented behavior, not merely the issue number or a generic "fix".
- Do not merge code changes until required CI is green for the code state being merged.

## Remote release control

MatterLeast releases can be created or retargeted without a local checkout through `.github/workflows/release-control.yml`.

- For a normal release request such as `release v1.6`, create the owner-only command issue with the exact title `[matterleast release]` and a JSON body like `{"tag":"v1.6","target":"master","move_existing":false,"run_release":true}`.
- To move an existing release tag, set `move_existing` to `true` and point `target` at the desired branch, tag, or commit SHA.
- To update only the tag without starting a release build, set `run_release` to `false`.
- The issue-trigger path is intentionally restricted to commands created by the repository owner. Do not weaken that guard.
- `release-control.yml` explicitly dispatches `.github/workflows/release.yml` after creating or moving the tag. Do not rely on the tag push itself to trigger the release because pushes performed with `GITHUB_TOKEN` do not start another push-triggered workflow.
- Release-control requests are serialized with workflow concurrency; do not remove that serialization without replacing it with equivalent protection against concurrent tag mutation.
- The workflow also supports manual `workflow_dispatch` with the same tag/target/move/run-release semantics.

## Documentation routing

The authoritative documentation index is [docs/README.md](docs/README.md). Top-level files in `docs/` are landing pages; detailed subsystem contracts live in focused subdirectories. Historical/obsolete architecture belongs under `docs/deprecated/`.
