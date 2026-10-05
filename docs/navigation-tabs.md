# Central navigation tabs

MatterLeast central tabs are a **presentation layer over semantic navigation**.
They do not create another authoritative channel/message model.

## Ownership

A tab stores a semantic destination:

- channel ID;
- optional thread root ID;
- a best-effort post/viewport bookmark;
- display title;
- whether the tab is pinned against ordinary channel-destination reuse.

Ordinary channel/DM/GM tabs reuse the one normal channel surface owned by
`ChannelTree` and `chatAreaStackedWidget`. Switching such a tab restores its
destination through the existing navigation path instead of constructing a
second `ChatArea` for the same channel.

A thread is different because it already has an independent `ChatArea`.
Moving a thread to a tab reparents that same widget from the right-hand thread
stack (or a detached window) into the central navigation surface. Attaching or
detaching it later reparents the same instance again. Do not clone a thread
model merely to change its presentation.

## Invariants

- One visible tab does not consume vertical space: the tab bar is shown only
  when there are at least two semantic tabs. Hiding the bar never hides the
  central destination itself; the normal channel surface remains the visible
  page of the central host.
- Tab titles use a leading star for their own unread domain. Channel/DM/GM
  tabs use `SidebarService::isChannelUnread()` or a channel mention. Thread
  tabs use their `FollowingModel::Entry::requiresAttention()`, including CRT
  counters and manual unread; they never inherit parent-channel unreadness.
  No known thread entry means no evidence for a star. Channel activity signals
  and `FollowingModel::changed` refresh these projections; reading a thread
  does not acknowledge its parent channel or another thread.
- A canonical destination (`channelId + rootId`) is unique in the tab model.
  Repeated middle-click, **Open in new tab**, permalink navigation or other
  navigation to the same destination activates the existing tab instead of
  creating a duplicate. This applies equally to channels, DMs/GMs and threads.
- Pinning is a property of the semantic channel-tab entry, not of a `ChatArea`
  widget. A pinned channel destination cannot be replaced by ordinary
  navigation. If a new channel is opened while its pinned tab is active, a new
  tab is appended; if the target already exists, that existing tab is activated
  as usual. When a thread tab is active, ordinary channel navigation may reuse
  an existing unpinned channel tab but must skip pinned channel tabs. Pinning
  does not prevent explicitly closing or moving a tab.
- Middle-clicking a tab closes that tab. Closing is deferred until the tab-bar
  mouse event completes because closing a thread can reparent its `ChatArea`.
- Docked thread, detached-window thread and tabbed thread are presentation
  states. They must not change backend membership, read-state authority,
  timeline sources, or thread identity. A transition out of a tab owns the
  QWidget reparent atomically: first release the thread from the tab surface and
  clear its tabbed presentation marker, then establish the new dock/window
  parent, and only then remove the semantic tab entry / activate its replacement.
  Toolbar-triggered presentation changes are queued to the next event-loop turn:
  never reparent the ChatArea while Qt is still dispatching the button's mouse
  release/grab sequence. Parent and window flags are changed in one setParent()
  transition so no stale native top-level surface remains behind.
- Browser-like Back/Forward history remains orthogonal to tabs. Tab switching
  may restore a recorded bookmark, but the tab model must not become a second
  post-loading state machine.

## Opening a new tab

The normal left click preserves existing navigation semantics. A middle click
is the browser-like modifier for opening the semantic target in a tab without
changing the sidebar selection first. This applies to channel/conversation rows,
Following/Attention channel or thread rows, internal Mattermost links in
message text, and the parent-chat link in a thread header. A middle click on the
thread header opens its root post in a tab through `openPostInTab()`, while the
ordinary left-click path remains `openPost()`. Permalinks still pass through
`AppNavigationService` so cold-post
resolution and reply-root loading happen before the tab is presented.

### Mattermost web URLs

Ordinary HTTP(S) links in message text are offered to `AppNavigationService`
before any browser fallback. A web URL is considered local only when its scheme,
host and effective port exactly match the configured Mattermost server and its
path is inside the configured server base path on a path-segment boundary. Host
substring matches, a different scheme/port, and lookalike base-path prefixes are
external. Keep this classification centralized; link widgets must not duplicate
origin checks.

Canonical `/<team>/channels/<target>`, `/<team>/messages/<user>` and
`/<team>/pl/<post-id>` routes are semantic navigation targets. Channel targets may
be either the Mattermost channel name or channel ID because server-generated URLs
can use either form. Left-click keeps the ordinary navigation path, while
middle-click uses the corresponding `open*InTab()` path. A same-origin URL whose
route is not recognized still falls back to the browser instead of inventing a
new client-side meaning.

Direct thread-row activation may present a tab before its root body is resident; it does not run the
left-click unread-resume query as a loading prerequisite. The ordinary thread source must bootstrap
that cold identity itself, as described in [Thread source](post-sources/channel-and-thread.md#thread-source).

Ordinary semantic navigation also reuses existing tabs. Once a navigation target
has resolved to its canonical `channelId + rootId` destination,
`AppNavigationService` asks the navigation UI to activate an existing matching
tab before continuing the normal semantic navigation pipeline. For channel,
DM and GM tabs, the **Pin tab** action in the ChatArea header menu marks the current
semantic tab as non-reusable. This action remains available when there is only one
tab and the tab bar itself is hidden. The header menu itself belongs to ChatArea so
it is shared by ordinary chats and every thread presentation; navigation-specific
actions such as Pin tab are injected/synchronized by NavigationUiController rather
than making ChatArea mutate the tab model directly.

The same header menu exposes contextual message search. It opens the ordinary
transient Search collection with a prefilled Mattermost `in:` modifier for the
conversation. Threads use their parent channel/DM/GM scope because Mattermost's
message-search syntax has no thread-root modifier.

A tab bookmark is passive revisit state and must never stand in for an explicit
post target. Explicit post/permalink navigation, including `openPostInTab()`,
activates the destination without restoring an older bookmark, preserves the
resolved post context, and then runs the same `MainWindow::openChannelPost()`
prepare/lock/highlight path used outside tabs. Ordinary tab switching continues
to restore bookmarks silently, without a highlight animation. Explicit
`open*InTab()` requests remain idempotent for the same canonical destination.

## Transient collection history

Saved, Drafts, Recent Mentions (when exposed), and Search remain singleton
transient central surfaces. They are browser-history destinations, not navigation
tabs. Showing one records its semantic collection kind in the process-local
Back/Forward history while the normal chat tab bar stays hidden.

Revisiting a collection through Back/Forward reveals the retained
`PostCollectionView` instance without calling its activation method again. This
is especially important for Search: returning from a result preserves the
current query and result set instead of issuing a new search. Reopening the same
collection while it is already current updates the current history position
rather than adding consecutive duplicate entries. Pinned messages are excluded
because that collection is embedded in a specific `ChatArea`, not a standalone
central destination.

Collection history must remain orthogonal to tab pinning and session restoration.
A transient collection never consumes or replaces a pinned chat tab, never enters
`NavigationTabsModel`, and is not written to the restart snapshot.

## Restart restoration

`NavigationUiController` saves a versioned semantic session through `MLOptions`,
scoped by server URL and login user ID. It records tab order, each tab's pin
state and the active tab, the ordinary central chat, docked threads and the
visible dock selection, and detached threads with their window geometry.
Browser Back/Forward history and transient collection/search surfaces are not
persisted.

Startup waits for channel memberships, team channels and rendered sidebar
category snapshots from the existing startup requests before replaying the
session. If initialization fails, the previous snapshot remains intact until
initialization can complete. Missing/inaccessible channels are skipped.
Channel population responses retain a `QPointer` to their original team;
replacing the team snapshot invalidates outstanding responses, including their
startup-counter completion. A late response must not mutate a replacement team
with the same ID or access the removed QObject. The startup counter is reset
for each new team snapshot. Qt 5.15.3 exposes this race in successive session
fixtures because HTTP completion order differs from newer Qt versions.
Presentation moves reuse the usual tab/dock/window ownership paths. The
snapshot is debounced after navigation/viewport changes and saved synchronously
when saving the main window or quitting; widget teardown must not replace it
with an empty session.

A viewport bookmark stores a post identity, never an estimated ordinal. A view
at the newest edge stores no post bookmark and resumes at the live edge.
Cold bookmarks resolve their bodies through `PostRepository` and use the quiet
viewport navigation path, without permalink highlighting. While a bookmark is
being resolved, it remains the saved identity and suppresses viewport read
acknowledgement of an incidental initial position. User scrolling or explicit
post/newest navigation cancels that pending restoration. Failed bookmark loads
leave the chat usable at its normal initial position.

## Extension point

A future collection feature should first decide whether it is a retained
transient destination or a true tab target. Retained transient destinations join
the typed Back/Forward history described above. A genuine tab target requires a
new `NavigationTabsModel` destination kind and explicit pin/reuse/session rules;
do not smuggle collection widgets into channel-tab entries or add another
top-level tab widget.

Navigation services are owned and discovered through their live Backend/MainWindow QObject children.
A process-static map keyed by raw owner addresses must not retain services after owner destruction:
a later session or integration fixture can reuse the same address. The navigation event filter must
only inspect the sidebar viewport for relevant mouse events, not during child-destruction events.
