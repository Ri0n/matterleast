# Windows auto-update: GitHub release channels

## Architecture and rollout

Port the **transactional version-switching design** of AnyKeep's Windows updater,
not its WiX/MSI extraction. MatterLeast distributes NSIS and an already existing
self-contained `matterleast-<version>-windows-x64-portable.zip`. That ZIP is
the canonical update payload; an extra update archive would be redundant.

**Status:** manifest generation and publishing infrastructure only.
The Windows client must **not** attempt unattended update installation until
the native launcher/updater and NSIS migration below are implemented and tested.

### Immutable binaries, mutable channel pointers

- Normal stable GitHub Releases: `v1.9`, `v1.10`, etc.
- Immutable nightly prereleases: `nightly-YYYYMMDD-HHMMSS-<shortsha>`.
- Two special prerelease Releases: `updates-stable` and `updates-nightly`,
  each containing only one file named `windows-x64.json`. The special tags
  must NOT move and do not trigger the regular `v*` release workflow.

Channel manifest schema 1:

```json
{
  "schema": 1,
  "channel": "stable",
  "platform": "windows-x64",
  "version": "1.9",
  "releaseTag": "v1.9",
  "minimumLauncherProtocol": 1,
  "package": {
    "format": "portable-zip",
    "url": "https://github.com/Ri0n/matterleast/releases/download/v1.9/matterleast-1.9-windows-x64-portable.zip",
    "size": 123456,
    "sha256": "64 lowercase hex digits"
  }
}
```

`version` is the CMake numeric version; `releaseTag` is the **unique build
identity**. Different nightly builds can have identical CMake versions.
For nightly, compare the sortable timestamped tag, not numeric version alone.
Stable uses numeric version comparisons. Channel changes must be explicit.

The publisher downloads the *actual published ZIP* and computes SHA-256 and
size from these final bytes. It does not accept arbitrary package URLs.
`.github/workflows/update-channel.yml` can be called by the Release workflow
to promote stable after successful publication, or run manually with
`channel` and `release_tag` for an existing published release.
Nightly publication/build is a separate CI task; expired workflow artifacts
are **not** real GitHub Release payloads.

Because `gh release upload --clobber` replaces a mutable asset, avoid relying
on long-lived caches of its download URL. The future client should resolve
`GET /repos/Ri0n/matterleast/releases/tags/updates-{channel}` each check,
select the current asset ID, then request that asset with appropriate caching
headers and redirects. Validate every manifest field and the exact trusted
repository/release URL; SHA-256 and size are mandatory, as are safe ZIP entry
paths and rejection of traversal/symlinks. Prevent downgrades. Transient 404
during pointer replacement should retry rather than corrupt installed state.

### Required remaining implementation

1. A tiny native `MatterLeastLauncher.exe` at the installation root, with
   `current.version`, `previous.version`, and
   `versions/<releaseTag>/MatterLeast/bin/matterleast.exe`. The launcher
   chooses a complete version. App data stays outside version directories.
2. Migrate the **existing flat NSIS install** (today `bin/matterleast.exe`)
   to the versioned layout. Keep Add/Remove Programs registration, shortcuts,
   user preferences and uninstallation functional. Never silently self-update
   an old installation before it has been migrated.
3. Background checks/download/staging with streaming file I/O, maximum
   manifest/download sizes, strict HTTPS and GitHub URL allowlist, version and
   format validation, SHA-256, atomic staging and path-safe ZIP extraction.
4. Native updater waits for the old process to exit, atomically switches
   `current.version`, starts the new build, probes for healthy startup, and
   rolls back to `previous.version` on failed launch or startup crash.
   Resume interrupted updates correctly.
5. UI: automatic checks, manual check, stable/nightly preference, progress,
   update prepared notification and update/restart. Portable legacy installs
   and MSIX/Store-managed installs must not be overwritten unsafely.
6. Windows CI installation, upgrade, rollback and uninstall tests, including
   intentionally broken new builds.

No copying of GPL-only AnyKeep implementation code into MatterLeast's
LGPL project without verifying the licensing rights; implement the same
architectural pattern independently.
