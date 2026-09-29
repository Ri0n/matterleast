# Central navigation tabs

MatterLeast central tabs are a **presentation layer over semantic navigation**.
They do not create another authoritative channel/message model.

## Ownership

A tab stores a semantic destination:

- channel ID;
- optional thread root ID;
- a best-effort post/viewport bookmark;
- display title.

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
- A canonical destination (`channelId + rootId`) is unique in the tab model.
  Repeated middle-click, **Open in new tab**, permalink navigation or other
  navigation to the same destination activates the existing tab instead of
  creating a duplicate. This applies equally to channels, DMs/GMs and threads.
- Docked thread, detached-window thread and tabbed thread are presentation
  states. They must not change backend membership, read-state authority,
  timeline sources, or thread identity.
- Browser-like Back/Forward history remains orthogonal to tabs. Tab switching
  may restore a recorded bookmark, but the tab model must not become a second
  post-loading state machine.

## Opening a new tab

The normal left click preserves existing navigation semantics. A middle click
is the browser-like modifier for opening the semantic target in a tab without
changing the sidebar selection first. This applies to channel/conversation rows,
Following/Attention channel or thread rows, and internal Mattermost links in
message text. Permalinks still pass through `AppNavigationService` so cold-post
resolution and reply-root loading happen before the tab is presented.

Ordinary semantic navigation also reuses existing tabs. Once a navigation target
has resolved to its canonical `channelId + rootId` destination,
`AppNavigationService` asks the navigation UI to activate an existing matching
tab before continuing the normal semantic navigation pipeline.

A tab bookmark is passive revisit state and must never stand in for an explicit
post target. Explicit post/permalink navigation, including `openPostInTab()`,
activates the destination without restoring an older bookmark, preserves the
resolved post context, and then runs the same `MainWindow::openChannelPost()`
prepare/lock/highlight path used outside tabs. Ordinary tab switching continues
to restore bookmarks silently, without a highlight animation. Explicit
`open*InTab()` requests remain idempotent for the same canonical destination.

## Extension point

Other central destinations (for example Saved, Drafts or search result
surfaces) should join the same semantic-tab layer rather than adding another
top-level tab widget or duplicating backend state. Until those collection
surfaces get their own tab target kind, opening one keeps its established
transient behavior: the normal central surface is revealed and the chat tab bar
is temporarily hidden, so an active tabbed thread can never cover Saved, Drafts
or Search.
