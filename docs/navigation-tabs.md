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
  when there are at least two semantic tabs.
- Explicit **Open in new tab** for a channel may create two tabs for the same
  channel. They are separate navigation/bookmark entries over the shared
  channel surface.
- A thread destination is unique while its `ChatArea` exists. Reopening the
  same thread in a tab selects the existing thread tab rather than trying to
  parent one QWidget into two surfaces.
- Docked thread, detached-window thread and tabbed thread are presentation
  states. They must not change backend membership, read-state authority,
  timeline sources, or thread identity.
- Browser-like Back/Forward history remains orthogonal to tabs. Tab switching
  may restore a recorded bookmark, but the tab model must not become a second
  post-loading state machine.

## Extension point

Other central destinations (for example Saved, Drafts or search result
surfaces) should join the same semantic-tab layer rather than adding another
top-level tab widget or duplicating backend state. Until those collection
surfaces get their own tab target kind, opening one keeps its established
transient behavior: the normal central surface is revealed and the chat tab bar
is temporarily hidden, so an active tabbed thread can never cover Saved, Drafts
or Search.
