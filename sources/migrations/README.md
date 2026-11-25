# Versioned migration steps

This directory owns **all compatibility with retired persisted formats**.
Normal account, network, and UI classes must not read legacy options or
reimplement old restore flows.

`MigrationManager` runs the steps in ascending version order. The latest
completed migration number is stored in `migrations/version` in application
settings; absence means version 0. A higher stored version is unsupported and
must not be overwritten/downgraded. The version is advanced only after a step
has completed its durable changes. Retrying a failed step must be safe.

| Step | Implemented (UTC) | Mechanism | Retirement guidance |
| --- | --- | --- | --- |
| **v1** | **2026-11-22** | `legacy-single-account-login`: migrate pre-multi-account `domain` / `username` / optional plaintext `token` (or existing keyring entry) to a verified durable `Account`. | Review removal when upgrades from builds predating 2026-11-22 are no longer supported. Do not remove solely because a later workspace JSON format exists. |

## v1 safety contract

1. On an existing multi-account layout, **do not automatically add** the
   process-global legacy login as an extra account. Preserve any unported
   plaintext token until the user explicitly recovers that identity.
2. With an empty catalog, read the old account's keyring entry asynchronously.
   Distinguish `NotFound` from `Unavailable`: a temporary wallet failure must
   not erase the old plaintext fallback or complete migration.
3. Attempt authentication using the old saved token only when the keyring
   reports `NotFound`. The server must confirm its user identity before a
   new durable account record may be adopted.
4. If the token came from settings, save the **verified** token into the
   platform keyring first. Never fall back to plaintext storage on write
   failure. Only then adopt the account, clear legacy fields and record v1.
5. Manual login may complete this step later, but only after
   `AccountManager::credentialsStored` confirms a successful keyring write
   for the **same** server/login pair. A different server/account cannot
   silently clear the old token.
6. Keep migration-specific network attempts and old settings names here.
   `LoginDialog` is only a manual credential editor; `AccountManager` owns
   normal authenticated sessions, independent of the old format.

7. Pinned-tab import (2026-11-25) is part of v1. Import only explicitly
   pinned destinations from the old account-scoped navigation snapshot into
   its View; do not import viewport bookmarks, ordinary tabs or open windows.
   The existing release migration version remains unchanged.

## Adding another migration

Append a monotonically increasing step to the dated descriptor array in
`MigrationManager.cpp`, add its version to the dispatch switch and increment
`LatestVersion`. Put step-specific logic here (or in a dedicated
`sources/migrations/vN_*.cpp` file as it grows), with tests for fresh installs,
upgrade paths, interruptions, future versions and repeated startups.
Never repurpose a released version number or move compatibility logic into
the account domain or the Widgets frontend.

## Diagnosing upgrade and saved-session restoration

Startup logs use two Qt categories:

- `mattermost.migration`: stored/current migration schema version, step
  execution, legacy keyring lookup result, authentication result and whether
  migration completed, was deferred or requires manual sign-in
- `mattermost.account.restore`: catalog load, active-profile restoration,
  per-account keyring status (`found`, `not-found`, `unavailable`), identity
  verification and adoption outcome

These are informational messages by default. When running a desktop build from
a shell, capture standard error alongside standard output. The log **must never
print passwords, bearer tokens, credential hashes, or plaintext legacy
settings**. A stable local account ID is logged only to correlate independent
restorations; no username or full server URL is logged.

The migration category is quiet after the schema is current except for an
explicit `No schema migration required` message; this is not equivalent to a
successful login. Normal session restoration is a separate phase.

For a regression report capture these lines, including the initial
`MatterLeast startup:` marker and any `mattermost.account.restore` warnings.
Avoid deleting account settings or keyring secrets before diagnosing
`not-found` versus `unavailable` versus a server-side rejection.

## Root URL keychain compatibility (2026-11-22)

The transition to per-Backend `NetworkSessionContext` automatically appended
`/` to a root server URL. The prior QtKeychain key computation treated
`https://server` and `https://server/` as different SHA-256 inputs, so normal
startup looked for the wrong entry despite the original session remaining
safely stored. The new `SessionTokenStore::keyFor` canonicalizes the root
slash and keeps the established `login-token-v1-*` key format.

The reader also accepts the root-slash variant written by intermediate
multi-account builds. No password/token migration heuristics are necessary,
and ordinary session restoration still verifies the remote user identity.
Only an explicit account logout removes matching keychain entries.
