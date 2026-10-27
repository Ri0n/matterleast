# Windows auto-update: GitHub release channels

## Architecture and rollout

Port the **transactional version-switching design** of AnyKeep's Windows updater,
not its WiX/MSI extraction. MatterLeast distributes NSIS and the existing
self-contained Windows portable ZIP as its canonical update payload.

**Status:** manifest generation and publication infrastructure only.
The Windows client must **not** attempt unattended updates until the native
launcher/updater and NSIS migration below have been implemented and tested.

## Compile-time update source and cryptographic trust

The client does **not** yet contain the Windows update launcher, downloader
or unattended installer. These CMake settings define an immutable update-check
policy and implement verification of update manifests for that future client.
No updater service is silently enabled by configuring these options.

- `MATTERLEAST_ENABLE_UPDATE_CHECKS` (default `ON`): compile-time switch.
  `OFF` leaves both update manifest URLs empty and makes the update
  authentication/checking policy refuse requests.
- `MATTERLEAST_UPDATE_MANIFEST_URL`: a single absolute HTTPS manifest URL.
  Use `{channel}` if stable/nightly have different locations; the
  application substitutes only `stable` or `nightly`. Without a placeholder
  both channels use the same URL. The default public source is
  `https://github.com/Ri0n/matterleast/releases/download/updates-{channel}/windows-x64.json`.
- `MATTERLEAST_UPDATE_VERIFY_KEY_FILE`: optional RSA public key PEM that
  requires a detached RSA PKCS#1 v1.5 / SHA-256 signature at `<manifest-url>.sig`.
  The signature authenticates **exactly the downloaded JSON bytes** before
  trusting URLs, hashes and package metadata inside the manifest.
- A build using `MATTERLEAST_PLUGIN_VERIFY_KEY_FILE` reuses that public
  key for update verification unless an explicit update key overrides it.
  If **any** plugin signing/verification key or update verification key is
  configured, CMake **requires an explicit non-default update URL**. This
  also applies if checks have been disabled. Corporate builds must not silently
  rely on the public GitHub endpoint.
- Public community builds without configured keys continue to use GitHub.
  Their manifests currently rely on HTTPS/GitHub, rather than a pinned RSA
  signature. A configured trusted build rejects unsigned, incorrect, malformed
  or tampered signatures (fail-closed).

Corporate example (key files are supplied securely by build infrastructure):

```sh
cmake -S . -B build \
  -DMATTERLEAST_PLUGIN_VERIFY_KEY_FILE=/secure/build/plugin-public.pem \
  '-DMATTERLEAST_UPDATE_MANIFEST_URL=https://updates.example.com/{channel}/windows-x64.json'
```

To completely disable checks at compile time use
`-DMATTERLEAST_ENABLE_UPDATE_CHECKS=OFF`. Disabling update checking does
not disable the independent native plugin signature verification.

### Publishing signed update pointers

The stable and nightly GitHub Actions release publishers support the optional
repository secret `MATTERLEAST_UPDATE_SIGNING_PRIVATE_KEY_PEM` (RSA private
PEM). When supplied, they sign `windows-x64.json` using
`scripts/sign-update-manifest.sh`, verify the signature locally and upload
`windows-x64.json.sig` **before** replacing `windows-x64.json`. Signature
and JSON are two assets, not an atomic transaction: a client must refuse
inconsistent pairs and retry rather than accepting unsigned data.

Once a channel has a signature asset, publishing without this signing secret
fails rather than silently downgrading the channel to unsigned. Nothing
containing a private key is compiled into the application or uploaded.

The signed manifest contains the package URL, exact SHA-256 digest and byte
size. Verifying its RSA signature, package hash/size, HTTPS restrictions,
release identity and downgrade policy protects the downloaded payload without
requiring an additional signature on the ZIP itself. Signature verification
**must precede all manifest-based downloads** in the future updater.

### Stable: immutable versioned packages and a mutable pointer

- Normal stable GitHub Releases are immutable: v1.9, v1.10, etc.
- The special prerelease updates-stable contains **only** windows-x64.json.
- Stable packages reside in their normal immutable v* Releases.
- Release workflow automatically promotes the stable pointer after successful packaging.
- Manual promotion (including emergency rollback) uses update-channel.yml.
- Stable releaseTag identifies the immutable release, and version is its numeric version.

Stable schema-1 example:

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

### Nightly: one rolling prerelease containing both packages and pointer

**Do not create a GitHub Release for each nightly build.** The sole mutable
updates-nightly prerelease carries windows-x64.json **and** the packaged
nightly assets (Windows ZIP and installer, Linux packages, debug packages).

- Each package has a unique build identity in its filename:
  matterleast-1.9-nightly-20261017-012300-abcdef123456-windows-x64-portable.zip
- The nightly buildId is nightly-YYYYMMDD-HHMMSS-<7-40 lowercase hex commit>;
  it is sortable and independent of numeric version. The numeric version can
  remain 1.9 across many nightly builds.
- In nightly manifests, releaseTag is **always** updates-nightly; buildId
  provides the actual immutable build identity. Package URLs refer directly
  to the uniquely named asset in that same rolling GitHub Release.
- Stable remains schema 1 without buildId. There is no Windows client using
  the old nightly contract; the first nightly publication supports migrating
  the previous schema-1 pointer with a nightly-* releaseTag.
- The old separate nightly-* Release is eligible for deletion **after seven
  days**, avoiding broken in-flight downloads from a previously seen pointer.

Nightly schema-1 example (illustrative sizes/digests):

```json
{
  "schema": 1,
  "channel": "nightly",
  "platform": "windows-x64",
  "version": "1.9",
  "releaseTag": "updates-nightly",
  "buildId": "nightly-20261017-012300-abcdef123456",
  "minimumLauncherProtocol": 1,
  "package": {
    "format": "portable-zip",
    "url": "https://github.com/Ri0n/matterleast/releases/download/updates-nightly/matterleast-1.9-nightly-20261017-012300-abcdef123456-windows-x64-portable.zip",
    "size": 123456,
    "sha256": "64 lowercase hex digits"
  }
}
```

#### Publication transaction and retention

.github/workflows/nightly.yml runs daily at 01:23 UTC, manually, or on its
own first deployment. It builds and tests all standard packages via packages.yml.

1. Check every package and rename all 12 using the unique buildId; never
   clobber package assets already published for another build.
2. Retrieve the currently published manifest and reject non-monotonic
   buildId promotion. An old build must not overwrite a newer pointer.
3. Upload all 12 uniquely named packages to updates-nightly.
4. **Verify every uploaded asset size and GitHub-reported SHA-256** against
   the local build. Abort without touching the pointer on any failure.
5. Upload windows-x64.json **last**. GitHub's --clobber replaces the
   pointer asset; this operation is not fully atomic, so readers must
   retry transient 404s during replacement.
6. Download the real published manifest again and verify its buildId, ZIP
   URL, size, digest and release metadata.
7. Only after that verification, remove expired packages: retain **at least
   three most recent builds** and **every build younger than seven days**.
   The active build is protected regardless of age. Expired legacy
   nightly-* prereleases are also removed, along with their tags.

Cleanup is guarded by a separate tested script, scripts/prune-nightly-assets.py.
It recognizes only the expected nightly package filename/suffix combinations
and specifically avoids deleting the pointer, stable releases, or unknown assets.
Partially uploaded packages of old nightly builds are eligible for cleanup
after retention expires. These safeguards protect both interrupted client
downloads and rollback to recent builds.

The special update tags must not move and do not trigger the regular v* release
workflow. Stable and nightly are separate channels; switching is explicit.

The updater should fetch GET /repos/Ri0n/matterleast/releases/tags/updates-{channel}
each check, identify the latest windows-x64.json asset ID, and request that asset
without depending on long-lived URL caches. Check schema, expected trusted
repository/release URL, HTTPS, buildId/releaseTag, SHA-256, size, ZIP entry safety
and downgrade prevention before staging. Transient 404 during pointer replacement
must be retried, never interpreted as a request to uninstall or downgrade.

### Required remaining Windows client implementation

1. A tiny native MatterLeastLauncher.exe at the install root, with
   current.version, previous.version, and
   versions/<buildId-or-releaseTag>/MatterLeast/bin/matterleast.exe.
   The launcher selects a complete version; user data stays outside these
   directories.
2. Migrate existing flat NSIS installs (today bin/matterleast.exe) to
   versioned layout without losing Add/Remove Programs registration,
   shortcuts, preferences, and clean uninstallation. Do not silently update
   an old installation that has not been migrated.
3. Background downloads/staging with streamed I/O, strict HTTPS allowlist,
   payload size limits, checksum enforcement, and path-safe ZIP extraction.
4. Native updater waits for the old process to exit, atomically switches
   current.version, checks new process health, and rolls back to
   previous.version on failed launch or startup crash; resume after power loss.
5. UI for manual/automatic checks, stable/nightly preference, progress and
   update/restart. Do not overwrite MSIX/Store-managed or legacy portable
   installations unsafely.
6. Windows CI tests for installation, upgrade, rollback and uninstall,
   including intentionally broken builds.

Do not copy GPL-only AnyKeep implementation into this LGPL project without
verifying licensing rights. Reimplement the architectural design independently.
