# Multi-server workspaces

Status: evolving multi-account implementation. The account-scoped Backend,
persistent QObject AccountManager, asynchronous independent restoration,
server overview and versioned credential migration have landed through #383.
The QtCore workspace tree projection is introduced in the next PR; its
Widgets/QML presentation and profile-aware connection transitions remain
separate stages. Full cache-backed offline browsing and non-modal sidebar
authentication notices are also future work.

## Goals and terminology

A **server account** is a durable local record with an immutable ID, server origin/base
path, server-assigned user identity (once known), display metadata and configuration. It
exists and is selectable even without an authenticated or constructed Backend. An endpoint
URL is not sufficient identity: two URLs can reach the same Mattermost database. Keep
credential material out of workspace configuration and in the existing secure token store,
scoped to the correct account identity at migration time.

A **connection profile** is an exclusive activation choice. Selecting a profile disconnects
accounts belonging only to the previously active profile and connects those belonging
to the new one. Every account in the selected profile stays connected, whether or not it
is currently visible. A profile can contain multiple **views**.

A **view** is a named projection of a nonempty subset of the profile's accounts. It
controls the visible sidebar, central tabs, navigation history and ephemeral UI state,
NOT connection lifetime or notification eligibility. One account may occur in several
views, even inside the same profile, but there is only one live backend per account.
View memberships are not a partition. Stable IDs, not names or list positions, are
used for profiles, views and accounts.

For example:

```text
Servers                         Connected        Visible on Channels
  Simultaneous
    Merged (Serv1, Serv2)       Serv1,2,3         Serv1 + Serv2
    Serv3 (Serv3)               Serv1,2,3         Serv3
  Serv4 (Serv4)                 Serv4             Serv4
  Serv5 (Serv5)                 Serv5             Serv5
```

Selecting `Serv1` or `Serv2` underneath `Merged` activates the **Merged view**
(not a third per-server view). Selecting `Serv3` activates its view, leaves Serv1 and
Serv2 connected, and hides them. Selecting Serv4 or Serv5 switches exclusive profiles
and disconnects the old profile first. Repeated selection of an already active view
does not reconnect anything.

View membership is many-to-many: later the user can add `All` (Serv1 + Serv2 +
Serv3), `Work` (Serv1 + Serv3), etc. Accounts shared by profiles are not duplicated;
the transition plan computes set differences, leaving common sessions alive. Explicit
"mutually exclusive" account endpoints which reach the same logical Mattermost must
not be connected twice inside one profile: the account/setup layer must either model
them as alternative endpoints for one identity or reject the conflicting combination.
Do not infer same database from similar URL strings. Multiple logins to one instance
may legitimately have different permissions, tokens and unread state.

## UX contract

- A compact icon-only **Servers** tab precedes Channels in the existing sidebar
  tab strip. Leave the existing one-server UI unchanged: no redundant server level
  in the tree. Whether the Servers tab remains visible with one account is a UI
  decision; it must not consume an extra navigation step to open a channel.
- Servers has a small toolbar (Add account, Add profile, Add view, Edit/Organize)
  consistent with the other sidebar tabs. The same commands are accessible through
  contextual menus and keyboard interaction. Group expansion is independent of
  activation; clicking a row's label selects its view/profile.
- The tree is an editor/projection over independent account/profile/view records,
  not an ownership hierarchy. One account can appear in several views. Drag/drop
  must express moving a shortcut vs changing membership unambiguously; it must
  never disconnect an account merely because a tree item was moved.
- Clicking an account row inside a view activates **that view**; this is essential
  because the account might also appear in another view. Highlight the active
  view, not every occurrence of its member accounts.
- Switching views restores their saved UI state without reconnecting; switching
  profiles applies the connection delta (disconnect previous-only, then connect
  new-only) and finally opens the remembered view for that profile.
- In Channels, if one account is visible, show the current categories unchanged.
  If more than one is visible, insert a top-level account node above its per-team
  categories. Cross-account DMs/GMs may share one recency-sorted list, with a small
  account provenance hint. Do not merge conversation bodies, contact identities,
  permissions, category data, unread state, or channel IDs.
- The Unread/Following/Attention list is a projection of visible accounts, preserving
  the existing sorting rules inside its domains. Hidden connected accounts continue
  to update their counters. The Servers tab shows a stable badge for unread/attention
  from connected accounts that are not visible. A brief new-event animation is OK;
  permanent blinking is not. No event-driven selection stealing.
- Desktop notifications originate from **all connected accounts**, including
  hidden ones. Receiving a notification never changes the view or focuses the
  window. Clicking the notification activates the owning profile/view (prefer
  last-used view containing the account), then opens the exact channel/thread/post.
  If no view contains the destination, offer an explicit, non-destructive fallback;
  never silently route to a different account with a matching channel ID.
- Selecting a result from global search or an external URL uses the same explicit
  account-scoped navigation. URLs are matched against configured origins and base
  paths; ambiguous matching is not resolved by whichever account happens to be
  active. Presenting an external link does not disrupt the current view.

## Single-owner account lifecycle after model separation

The next implementation stage consolidates the former `AccountRegistry` and
`AccountSessionManager` into one `AccountManager` with QObject-owned durable
`Account` instances. Each `Account` survives disconnects and owns at most
one live `Backend`; during restoration it may own a separate temporary pending
`Backend`. New account candidates temporarily belong to the manager until a
remote user identity is verified and adopted.

`WorkspaceLayout` is the sole data/serialization topology for account identity
and profile/view memberships. `AccountListModel` is a QtCore-only presentation
projection with stable account IDs; neither it nor the Widgets stack owns
credentials or network sessions. `MattermostApplication` now only connects
Widgets/tray to account signals and orders view destruction before backend reset.

`UserActivityMonitor` observes connected sessions from `AccountManager`
without requiring a second session registry. Keychain loading, token/user
verification, connection states, retry actions and explicit logout are
account-domain operations. Model role updates remain incremental.

## Versioned settings migrations

Before account restoration, `sources/migrations/MigrationManager` applies
versioned, dated compatibility steps based on `migrations/version`. The initial
`v1` step (implemented **2026-11-22**) handles pre-multi-account
`domain`/`username`/`token` settings. Legacy authentication and keyring
fallback no longer belong to `LoginDialog` or `BackendLoginData`.

Migration results never overwrite future-version settings, never erase an
unmigrated plaintext token after a temporary failure, and only advance the
stored version after verified adoption and required secure storage. The step
inventory and removal criteria live in
`sources/migrations/README.md`.

## UI-independent models and multiple frontend technologies

The business-level account and workspace data model must not depend on
`QWidget`, `QApplication`, window lifetime, or Qt Quick. Desktop Widgets and
future mobile QML are alternative presentations of the **same** model.

- `WorkspaceLayout` remains the pure, versioned configuration topology:
  stable account IDs, connection profiles, view memberships and current view.
  It never starts a network client and cannot depend on the UI framework.
- An eventual single `AccountManager` owns durable `Account` QObjects and
  their optional Backend sessions, statuses and login/reconnect transitions.
  Do not retain separate public account registration managers or parallel
  account-status tables in the application. Sessions must be QObject-owned.
- `AccountListModel` is a **read-only QtCore QAbstractListModel projection**
  suitable for Qt Widgets and QML. Its stable roles include `accountId`,
  `displayName`, `server`, `status` and `selected`. It does not duplicate
  credentials or take ownership of a Backend; account data and connection
  policy stay authoritative in the manager.
- `WorkspaceTreeModel` is the UI-independent `QAbstractItemModel` projection
  of **profiles → views → account shortcuts**. A single account may appear in
  several views but is never duplicated in `AccountManager`; its connection
  state is read from `AccountListModel`. Selecting a view changes only the
  affected `active` roles, and changing a connection updates all matching
  account shortcuts without resetting the tree. Structural topology edits may
  trigger an explicit reconstruction. The model is **read-only**: the
  presentation dispatches user commands to `AccountManager` rather than
  mutating Qt model indexes or reparenting nodes.
- `AccountOverviewPage` (Widgets) binds to this model and emits selection,
  connect and sign-in intents. It must not cache a second list of accounts,
  decide when to reconnect, mutate account identities, or directly read
  secrets. A QML view can consume the same model and trigger equivalent
  manager commands through a frontend-agnostic action API.
- The permanent MainWindow presents account-qualified View state directly;
  no account-owned window or separate startup workspace container exists.
- Use row-level `dataChanged`, insertion, removal and moves for small
  state changes. Avoid unconditional `beginResetModel` and re-creating
  entire widget trees on a connection-status update.
- `MattermostApplication` owns application lifecycle and wiring only.
  Connection retries, account lookup, persistence and cross-server routing
  must migrate out of the Widgets/application orchestration and into
  UI-independent account services.

Delivery: #383 consolidates the account lifecycle into one manager; the QtCore
list model and ID-keyed Widgets frontend were introduced in #382.

## Profile activation and connection reconciliation

`AccountManager::activateView(viewId)` (or account selection resolving to a
different view) now makes a profile/view selection without giving Widgets
ownership of the connection policy. `WorkspaceLayout::transitionTo()`
validates the target profile and its desired memberships. The runtime
`AccountManager` then reconciles **actual live/pending Backends** against
that selected profile; this is intentional, because a remote login can
finish asynchronously after the declarative plan was computed.

- Moving between views **inside the same profile** changes only the view
  projection and selected account. Every profile member, even a member hidden
  by the selected view, remains connected and notification-eligible.
- Switching exclusive profiles first cancels pending restores of accounts
  not present in the new profile. For their live Backends,
  `AccountManager` stops network transports and signals
  `profileSuspensionRequested`. The Widgets application prepares and
  destroys the associated account view **before** acknowledging with
  `finishProfileSuspension`, which clears Storage and releases the Backend.
- New profile members are restored **only after all outgoing Backends have
  been released**. Accounts shared across old/new profiles retain their same
  Backend, Storage, notifications and keyring identity. An interrupted or
  rapidly reversed transition reconciles against the **latest** selected
  profile, not an outdated queued connect list.
- Profile suspension is not explicit logout: do **not** send
  `/users/logout`, delete the saved keychain token or remove the durable
  Account record. A later activation can reconnect normally. Accounts that
  require credentials or have an unavailable keyring remain visible with
  their error state, awaiting explicit retry.
- Startup restores the entire active profile, **not** every saved account.
  View selection does not need a network connection.

The existing account workspace stack still renders one account at a time.
**Do not construct a parallel Channels model:** the current `ChannelTree`
already owns an extensively reconciled `QTreeWidget` model and retains
individual `ChannelItem` rows through category moves. It also suppresses
spurious navigation during reconciliation, defers structural updates during
drag-and-drop, and keeps existing chat pages alive. Multi-account work must
adapt that implementation incrementally rather than duplicate its category,
unread, DM/GM and selection algorithms.

The first account-scoping step marks existing `SidebarItem` rows with a
stable local `AccountIdRole`, including rows populated asynchronously, without
replacing or resetting the current item model. A future merged view must
qualify the existing team/channel indexes and their navigation with account
identity, separate ChatArea ownership from tree presentation, and retain the
tested in-place reconciliation and interaction behavior. It is not yet a
multi-account Channels UI. The mixed DM/GM list has a view-local `directMessagesSorting` setting:
`recent` (default) or `alpha`. The setting is persisted in the existing
workspace catalog; older catalogs default to `recent`. Single-account views
continue to use the server category's current sorting, while a merged view
must apply its own setting to the combined entries **without modifying any
server's category sorting**. A view-local manual order is intentionally not
specified: server-side manual lists have no meaningful inter-server order.
Do not maintain a separate View-scoped DM/GM item model. The combined DM/GM
branch must be materialized by the same sidebar model and reconciliation
mechanism as regular channel rows. This avoids two independently updated
projections of the same channels; the existing persisted sorting preference
is retained for when the combined branch is implemented.
Shared Unread/Attention and per-view navigation remain subsequent work.

## Per-view UI state (essential invariant)

**Each view has its own central tabs, active tab, Back/Forward stacks and sidebar
presentation state.** Switching away hides these tabs and history; returning restores
them. Tabs and Back/Forward do not automatically switch to another view. Ordinary
navigation within a view stays within that view.

An explicit cross-view action (notification click, selecting a server/view, or
account-scoped search result) switches to the target view first. An inactive
view's open tabs must not leak into the current tab bar, nor may Back/Forward
cross its boundary. A destination that exists in several views can legitimately
be open as separate *presentation tabs*, backed by the same server-side channel
data; deduplication is scoped to the view. Tabs retain their per-view pins,
scroll anchors, docked thread presentation and selection.

The lifetime of chat data and backend subscriptions is **per account**, not per
view. A view owns semantic UI state, not independent copies of message history.
Detach/hide presentation without destroying shared models or clearing drafts when
switching views. If a profile becomes disconnected, keep its view snapshots;
restore after reconnection and membership admission. Missing channels are handled
by the existing restoration rules rather than leaking a stale widget from another
account. Detached thread windows need an explicit visibility policy when switching
views; initially they remain tied to their originating view and must not be
mistaken for the active view.

A QtCore `ViewNavigationSession : QAbstractListModel` is now owned by
`AccountManager` for each immutable view ID. It holds lightweight semantic
tabs, pinned flags, bookmarks and bounded Back/Forward state. Its destination
identity is `{accountId, channelId, threadRootId}`, so two servers may use
identical channel IDs without collisions. Sessions survive Backend teardown
and switching away from the view; neither a `Backend` nor a `ChatArea` is
duplicated for a second view. Widgets and QML can consume the same roles.

`ViewNavigationSession::snapshot()/restoreSnapshot()` now provide a
QtCore-only transfer boundary for a Widgets presenter. An incoming snapshot is
validated before touching the model and reconciled with incremental row changes
(including moves), without resetting listeners or allocating chat widgets.
This is a transfer primitive, **not** an alternative long-lived owner of tabs.

The Widgets `NavigationUiController` now transfers its semantic tabs,
viewport anchors, bounded navigation history and visible docked-thread
location to the `AccountManager`-owned session for **single-account views**.
Switching between two such views of the same account preserves independent
navigation state without duplicating a Backend or Storage; detached threads
remain associated with their originating view while the Backend is live.
The existing per-account restart snapshot is adopted into an initially empty
view session. The common snapshot is not yet a persisted per-view restart
format, so saving all views across process restarts is still pending.

Merged views use account-qualified destinations in the same MainWindow and
`NavigationUiController`. Switching the active View reconciles its own tab
snapshot rather than retaining window-global tab contents.
Continue to obey the tab/thread reparent and viewport-anchor invariants in
[Central navigation tabs](navigation-tabs.md).

Existing restart snapshots are scoped to server URL + login ID. During migration,
adopt them into the initial/default view without losing pinned tabs, active tab,
thread state or drafts. Back/Forward is currently process-local; per-view history
can remain process-local initially, but it may never be silently shared.

## Runtime ownership and lifecycle

The AccountManager owns durable Account QObjects, each with an optional
QObject-parented Backend. Accounts persist across disconnects and have stable
local IDs. Never confuse the lifetime of a Backend with the existence of its
account record.
Never swap one global Backend between URLs: late callbacks, cache keys, requests,
reaction/emoji registries and notifications must remain scoped to their originating account.

The desktop has exactly one MainWindow for the entire application, including
startup with no connected Backend. Its existing ThinSplitter, sidebar tab bar
(Servers, Channels, Unread, Attention), menus, toolbars and central navigation
surface are application-owned. Switching a View changes its projected contents and
semantic state, not the MainWindow or splitter. Window geometry and the sidebar
width are global presentation settings. Account-specific actions resolve the owning
account explicitly; an active chat can belong to a different account from the
currently selected sidebar account.

MainWindow constructs its permanent layout before authentication. Navigation
observes account attachment and detachment explicitly rather than polling.
Plugin overlays attach once to this surface. The single Channels tree remains
in its original layout; switching views does not reparent it or replace it with
another tree. No per-account geometry or sidebar width is written or restored.
Preserve per-view navigation semantics without duplicate network clients or
post caches.
Transport and cache services operate on account-scoped data even when the view is hidden.
Any singleton keyed by `Backend&` must be audited for lifetime/address reuse. Async
completions must be fenced by stable account ID and session generation, never by whichever
account happens to be visible. Explicit account removal/log-out must cancel pending work
before destroying UI referencing the session, preserving the logout ordering.

Server-origin event handling updates an owning account's state. Derive the UI
projection from that state without resetting complete models when rows/roles
suffice. No delayed timer to hide selection, layout or focus glitches.
Treat unread and read acknowledgment as account scoped: a message visible in
one view may update shared server read state, but simply switching/hiding a
view must not mark its messages read.

Authentication secrets never enter JSON workspace layouts, debug logs or UI
session snapshots. Existing keyring entries must remain usable; avoid destructive
migration of the legacy single-account options before a new layout has been
validated and durably saved.

## Durable accounts, offline workspaces and authentication recovery

The first runtime implementation (#380–#383) introduced durable
credential-free Account records, a persistent Servers overview, independent
asynchronous keyring restoration, per-account startup error states and
account-scoped password/token reauthentication. It intentionally does **not**
retain Backend::Storage or loaded post widgets after a connection is closed.
The full cache-first offline UI and sidebar footer notifications are separate
remaining steps.

The design follows the distinction between a persistent Psi account and its optional
`XMPP::Client`: a saved MatterLeast account exists independently of its optional
`Backend` network session. **The user selects an account or view, not a socket.**

- **AccountRecord / registry**: small persistent, versioned metadata keyed by a stable
  local account ID. Origin/base path, user ID, display name, desired connection policy,
  and per-account settings survive restarts. An unconfigured login candidate must not
  appear as a fully established account. Profile/view memberships reference these IDs.
- **Account connection state**: an independently observable state per account, distinct
  from visibility: `Disconnected`, `Connecting`, `Connected`, `Reconnecting`,
  `AuthenticationRequired`, and `ConnectionError` (names may evolve). A transient
  timeout, WebSocket disconnect, HTTP 5xx, or permission-specific 401 must not
  automatically mean invalid credentials. Only a confirmed account-authentication
  rejection triggers `AuthenticationRequired`.
- **Backend**: created when attempting a connection and destroyed when appropriate,
  with QObject parentage and account-scoped transport services. Never store secrets
  or session identity in a process-global current-account setting. Do not destroy a
  permanent account or its presentation merely because its Backend reset/logged out.
  Keep distinct **disconnect/retry**, **reauthenticate**, **explicit log out**, and
  **remove account** operations.
- **Cache / offline projection**: show the saved account and available disk-backed
  channels/contacts, post history, drafts, tabs and navigation even without network
  connectivity. Do not invent complete data from incomplete cache snapshots or mark
  cached posts read just because a view is opened offline. Clearly distinguish
  unavailable server actions. Existing `Storage` currently belongs to `Backend` and
  `Backend::reset()` clears it: introduce a durable cache-facing data boundary
  incrementally, without duplicating resident models or holding invalid Storage pointers.
- **Login experience**: opening MatterLeast with registered accounts displays the
  normal workspace/Servers UI immediately, even if every connection fails. Restore
  saved sessions independently and asynchronously; never show a modal LoginDialog
  merely because one of several accounts needs attention. On first launch with no
  configured accounts, offer Add account/login.
- **Credential recovery**: a compact, accessible, non-modal sidebar-footer banner
  identifies the affected server and user and opens a small account-specific
  password/token editor. The saved origin/user are prefilled and should not be
  silently replaced. A successful login returns to the same account/workspace and
  preserves tabs, navigation and drafts. The credential editor must not own network
  retries, keyring operations or Backend lifetime.
- **Sidebar notices**: share a presentation container with the existing update banner,
  not a proliferation of standalone fixed widgets. Multiple simultaneous account
  failures must remain discoverable in the Servers list; the footer can show a concise
  current/aggregate prompt without stealing focus or changing the selected view.
- **Credential safety and migration**: the current `BackendLoginData::loadFromOptions()`
  reads a process-global `domain` and `username`. Treat this as a **one-time legacy
  import**, never as a restore source for every account. Continue using the secure
  `SessionTokenStore`, with account-scoped keyring lookup and asynchronous read/write;
  do not copy tokens into JSON/layout/cache or expose them in logs. Persist the
  validated new registry before retiring the legacy settings.

Ownership boundaries:

```text
AccountRegistry (durable account IDs, settings and profiles/views)
  ├── Account A ── AccountData/Cache ── Workspace/View state
  │     └── Backend* (optional, QObject-owned by session manager)
  └── Account B ── AccountData/Cache ── Workspace/View state
        └── Backend* (optional, QObject-owned by session manager)
AccountSessionManager (runtime connection/session facade)
```

The registry should remain simple: move network retries into session-specific
controllers and keep cache/storage work behind its own interface. Do not make the
account manager depend on every per-account stateful service. Avoid full model
resets/recreations when only state roles or a few affected rows change.

Initial acceptance criteria: two stored accounts survive restart; one can fail
401 without a modal dialog or affecting the other's traffic; offline data remains
navigable; successful reauthentication restores the same account identity, tabs
and drafts; explicit logout does not immediately auto-restore a rejected token;
legacy single-account credentials migrate once with no plaintext token; rapid
connect/disconnect and delayed callbacks cannot mutate another account.

## State, configuration and transitions

The workspace layout owns stable account/profile/view IDs, display names,
membership, active view ID, and eventual account endpoint metadata. A profile's
connection membership is a distinct list from a view's visible-account subset.
Layouts are versioned and validated atomically: reject unknown references,
duplicate IDs, duplicate memberships, empty/nonexistent active view and invalid
cross-profile view references. Invalid or future-version settings do not overwrite
the previous working configuration.

The pure `workspaces/WorkspaceLayout` layer computes a transition by taking the
difference of the source/target profiles' account sets. Its `disconnect` list
is processed **before** its `connect` list; changing views within one profile
produces empty deltas. The pure layer never opens sockets, persists credentials
or mutates widget state. The runtime owner applies transitions and reflects
partial connection errors per account. The selected profile describes the
*desired* connections; actual connection state is tracked independently.
A failed connection must not fabricate a successful "connected" badge.

Persistence goes through `MLOptions`; use one versioned layout value and
separately keyed, versioned per-view UI snapshots. Names are editable, IDs
must survive reordering and rename. A profile with no view or a view with no
visible members is not a valid user-selectable configuration. Keep a recovery
path for a newly added/unconfigured account rather than constructing a half-
valid active profile.

## Delivery order / checkpoints

1. **Data foundation**: pure, tested layout model, versioned JSON and
   exclusive-profile/view transition planning. No production behavior change.
2. **Session manager**: multiple independently owned Backend contexts, secure
   account credential lookup and single-account compatibility/migration.
3. **Workspace UI**: Servers tab with toolbar, menu, add/edit/delete/reorder,
   view selection, profile switching and connection/error indicators.
4. **Per-view navigation**: isolate tab models and Back/Forward histories;
   ensure thread dock/window and restart snapshots obey view identity.
5. **Aggregated sidebar**: account-scoped tree and recency-merged DM/GM plus
   Unread/Attention projections, preserving incremental model updates.
6. **Cross-view routes**: notifications, account-aware search, permalinks,
   restored navigation and tray aggregation. Current #373–#375 work is a partial
   implementation using one account per view.
7. **Durable account registry and migration**: stable account IDs, versioned
   metadata, one-time import from legacy options, independent restoration policy;
   unit tests for corrupt settings, duplicate identity and cold startup.
8. **Account-local connection state**: separate connection from account existence;
   independent asynchronous session restoration, retry/backoff, confirmed 401
   transitions, safe reconnect and backend destruction. Preserve healthy accounts.
9. **Non-modal credential recovery**: refactor LoginDialog into an account-scoped
   password/token editor; reuse a sidebar-footer notice surface; no startup login
   dialog while at least one account is configured.
10. **Offline account data and presentation**: cache-backed channels/contacts and
    posts with truthful freshness/coverage, resilient views, drafts and histories.
    Split the current Backend-owned Storage boundary incrementally; no premature
    second copy of the full in-memory model.

Regression matrix: one server, two views sharing one account, three concurrent
servers with two disjoint views, overlapping memberships, profile switches with
a shared account, disconnected active view, duplicate channel/user IDs across
servers, rapid switch during HTTP/WS callbacks, pinned tabs, Back/Forward,
docked/tabbed/detached threads, drafts, unread+Attention, notification click,
cold startup restoration, and failed/partial login.

## Combined Unread and Attention projections

The two queue widgets subscribe to the existing FollowingModel for every live
account in the active View. Account-qualified row identities prevent remote
channel/thread/user ID collisions. Unread retains its source priority groups
and Attention retains its chronological order across all contributing accounts.
Refresh reconciles the row delta, preserving existing items and selection.
Switching View membership disconnects obsolete subscriptions; no account's
read cursor belongs to the queue widget. Context menus and deferred navigation
resolve the exact row owner rather than the profile displayed in the header.
AccountTrayBadgeMonitor subscribes to every authenticated account's existing
activity models at application level. The visible Attention count is used only
for the View's tab/title; hiding a connected account cannot clear its tray
badge. Account unregistration removes its own contribution, without creating
or updating a sidebar widget.

## Application notification routing

`AccountNotifications` observes all authenticated sessions independently of
Widgets. Desktop post suppression queries the exact source Backend and focused
conversation; a notification click selects the source account and navigates
through its own AppNavigationService. Incoming call identities include the
source session, and all invitations share one ringtone timer and lazy sound
effect. Disconnecting a session stops only its ringing and notification
targets. No notification subscriptions belong to MainWindow.

## Account-owned startup data

The AccountManager starts the authenticated Backend's catalog, membership,
webapp-plugin and pending-send projections once. `SidebarService` owns the
category snapshot and realtime DM/GM admission, including when no tree exists.
Verified replay refreshes membership/preferences/categories on the account,
not on any View. ChannelTree subscribes to the cached snapshots. When a team
vanishes, detach its row from the Qt item model before destroying the derived
TeamItem: synchronous removal observers must see the intact item, not its
partially destroyed QTreeWidgetItem base. If the current selection belongs to
that branch, clear it before removing any rows, so Qt does not choose a new
current index during structural removal. Other account rows and selections,
and the model itself, remain unchanged. Saved
collections subscribe directly to their own Backend's flagged-post events.
The standalone constructor still starts the same idempotent startup path for
widget fixtures; production session initialization does not require a window.

Account-owned deferred profile requests are invalidated when the session ends.
The logout reply and timeout share a single completion path; the final reset
runs on the Backend event loop outside the HTTP reply signal so it cannot
destroy the emitting reply or its network manager on that signal's stack.

## Permanent desktop window

The application creates one MainWindow before authentication. The Servers tab
is available even with no account. A selected View changes the native tree's
account roots and the navigation session, not the window or its splitter.
Account connection and suspension attach/detach account-qualified UI sources;
closing one connection releases its widgets before Storage is reset. The
application-wide sidebar splitter state is saved under the existing global
key. Restore it after the navigation content host has replaced the original
stack in the splitter; preserve the central stretch factor and reject a zero
(or minimum one-pixel) conversation pane even before the first account has
connected. Never persist a collapsed application conversation pane. Widget
integration tests exercise real sidebar clicks and verify visible ChatArea
geometry, not merely the current stack page. The standalone one-Backend
constructor remains for widget tests; the obsolete workspace stack and separate
startup page have been removed. Startup tests exercise the real Servers tab.
