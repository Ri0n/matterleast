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
- Windows 2025 with Qt 6.11.2.

Do not add installer, DEB, RPM or artifact-production work to the commit CI. Packaging lives in `.github/workflows/packages.yml`, which is invoked manually or by the tag-driven release workflow.

The packaging workflow owns release artifacts for Ubuntu 22.04, Ubuntu 24.04, Ubuntu 26.04, Fedora 44, a portable Linux AppImage and Windows. Ubuntu 24.04 is packaged only with Qt 6. The release workflow must verify the complete expected artifact set before publishing a GitHub release.

The native DEB packages deliberately use each distribution's Qt. The AppImage has a different role: it bundles the newest Qt version that is still supported on the AppImage build baseline. It is built and smoke-tested on Ubuntu 24.04/XCB, currently with Qt 6.11.2, so users affected by old distribution Qt/XInput bugs have a self-contained alternative without changing their system Qt. Keep the AppImage build host at Ubuntu 24.04 while that is the oldest supported AppImage target; AppImage does not make glibc backward-compatible.

The AppImage toolchain is downloaded by immutable GitHub release-asset IDs and checked against recorded SHA-256 digests. When updating linuxdeploy or linuxdeploy-plugin-qt, update the asset ID and digest together. The Qt deployment must include the XCB and Wayland platform plugins, SQLite driver and SVG image plugin. Qt 6.11 consolidated the Wayland QPA entry point into `libqwayland.so`; do not use the older `libqwayland-egl.so` / `libqwayland-generic.so` names from older linuxdeploy-plugin-qt documentation. MatterLeast only needs the SQLite Qt SQL driver; prune the other `libqsql*.so` plugins from the Qt deployment source before running linuxdeploy, because official Qt binaries also contain drivers whose external vendor libraries (for example Mimer SQL) are not part of the AppImage and must not become accidental runtime dependencies. The Ubuntu 24.04 smoke test intentionally forces XCB because the package also serves as the workaround path for old XInput behavior.

Linux desktop/AppStream metadata uses the reverse-DNS ID `io.github.Ri0n.MatterLeast` for the desktop file, icon and AppStream component. The AppStream developer ID is separately `io.github.ri0n`: developer IDs must contain only lowercase ASCII letters, digits and punctuation even when the corresponding GitHub login uses uppercase characters. Do not reintroduce screenshots that point at the pre-fork `nuclear868/mattermost-qt` repository; only add AppStream screenshots when the image is maintained at a stable MatterLeast-owned URL. Keep AppStream metadata valid under `appstreamcli` because linuxdeploy's AppImage plugin treats validator warnings as packaging failures.

## Documentation

- Architectural decisions and hard-to-rediscover invariants belong in the repository, not only in chat history.
- Start from [README.md](README.md) and follow only the documentation path relevant to the task.
- Keep landing pages short and put details in focused child documents.
- Avoid duplicating the same rule across several files; link to the authoritative location instead.
- Move obsolete design material to `deprecated/` so old and current contracts cannot be confused.

## Scope discipline

Prefer changes at the layer that owns the behavior. In particular, presentation-only behavior should normally be modeled in presentation/source layers rather than by corrupting authoritative backend state or hiding rows with layout hacks.

Keep public API surface small. Helpers and parsing/implementation details that do not need to be public should remain private to their implementation.
