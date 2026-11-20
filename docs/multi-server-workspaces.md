# Multi-server workspaces

Status: architecture decision and staged implementation. The initial implementation in this
branch provides the pure workspace layout/transition model; runtime multi-session ownership
and the sidebar are subsequent stages. Do not interpret the presence of this model as
multiple live Backends being supported yet.

## Goals and terminology

A **server account** is one authenticated session (Mattermost origin and account identity);
it has an immutable local ID. An endpoint URL is not an identity: two URLs can reach the
same Mattermost database. Keep credential material out of workspace configuration and in
the existing secure token store, keyed by the account identity at migration time.

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

The current `NavigationUiController` has one `NavigationTabsModel` and one pair
of process-local Back/Forward stacks. The migration must move these into a
`ViewNavigationSession` owned by view ID. Keep per-view history separate even
if the underlying account/channel IDs match. Use semantic identities such as
`{accountId, channelId, threadRootId}`, never a bare `channelId` globally.
Continue to obey the tab/thread reparent and viewport-anchor invariants in
[Central navigation tabs](navigation-tabs.md).

Existing restart snapshots are scoped to server URL + login ID. During migration,
adopt them into the initial/default view without losing pinned tabs, active tab,
thread state or drafts. Back/Forward is currently process-local; per-view history
can remain process-local initially, but it may never be silently shared.

## Runtime ownership and lifecycle

Currently `MattermostApplication` owns one `Backend` and one `MainWindow`; the
backend owns Storage/HTTP/WebSocket and account-specific services. Introduce an
account/session manager owning one Backend context per connected server account.
Avoid swapping a single global Backend object between URLs: late callbacks,
cache keys, requests, reaction/emoji registries and notification routing could
otherwise attach to the wrong server.

Keep a single MainWindow hosting a projection of all active view accounts.
Transport and cache services continue to operate on account-scoped data even
when their view is hidden. Any global singleton/service keyed by `Backend&`
must be audited for lifetime and address reuse. Async completions must be
fenced by account/session generation, not by current visible account.
Connection deactivation must cancel pending work before destroying UI that
references it, preserving the existing logout ordering.

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
   restored navigation, tray aggregation, offline and reconnect handling.

Regression matrix: one server, two views sharing one account, three concurrent
servers with two disjoint views, overlapping memberships, profile switches with
a shared account, disconnected active view, duplicate channel/user IDs across
servers, rapid switch during HTTP/WS callbacks, pinned tabs, Back/Forward,
docked/tabbed/detached threads, drafts, unread+Attention, notification click,
cold startup restoration, and failed/partial login.
