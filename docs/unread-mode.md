# Sidebar Unread filtering

There are two independent Unread controls. Neither creates a new unread model
nor marks messages read.

## Unread tab (issue #277)

The second left-sidebar tab replaces **Following** with **Unread**. It has
three compact, independently persisted checkboxes directly above the list:

| Checkbox | Default | Meaning |
| --- | --- | --- |
| **Following** | On | Existing `FollowingModel` queue, including unread DMs/GMs, explicitly followed threads even when read, and synthetic root mentions. Its sorting, thread navigation, read/resume cursors and selection retention remain unchanged. |
| **Channels** | On | Additional public/private channels that are joined/tracked and currently unread **without an outstanding personal mention** according to `SidebarService`. No new persistent Following membership or read cursor is created. DMs/GMs are not introduced by this source. |
| **Muted** | Off | Allows muted unread channel entries into the Channels source and muted conversation entries already provided by Following. The old explicit-followed-thread exception remains unchanged. This is an inclusion modifier, not an independent source of rows. |

The checkboxes are stored in `MLOptions` under
`sidebar/unreadView/{following,channels,muted}`, with defaults `true/true/false`.
Changes immediately rebuild the view and clear any obsolete selection-retention
snapshot; they do not change the Attention tab or the server's mute/read state.

The projection is a union ordered by the existing Following unread/activity
priority. Stable identity is `c:<channelId>` for conversations and
`t:<threadId>` for threads. When an unread channel is already represented
by Following (for example, an explicit Mark as unread), it appears **once**;
the model-backed Following entry wins and retains its existing resume semantics.

Clicking a Channels-only row asks the server for its unread post and navigates
to the returned post (falling back to the channel when the server cannot supply
one). Unlike a Following row, this view-only entry does not establish a
`FollowingModel` resume cursor. Middle-click opens its channel in a new tab.
Selection retention is ephemeral and never means a post was read. Filtering
by text continues to apply to the list.

## Channels toolbar's unread-only toggle

The *Channels* tab has a separate unread-only toolbar toggle, with its own
text-filter behavior. It does not control the new Unread tab checkboxes.

**Settings → Sidebar → Unread mode → Ignore while filtering** is enabled by
default: typing a sidebar text query suspends the unread gate on the Channels
tree to let the user find matching read channels. Clearing the query restores
the gate. If disabled, text matching and unread matching are intersected:

```text
matchesText && matchesUnread
```

The toggle is a presentation filter and never changes Mattermost read state.
The active channel can temporarily remain visible even when it is read, to
prevent its selected row from disappearing mid-click. Virtual destinations are
not removed by the unread gate, but still obey the explicit text filter.

`SidebarService` owns authoritative channel unread/mute state;
`FollowingModel` owns the followed conversation/thread queue.
Neither the Channels toolbar nor the Unread tab may create a competing read
state machine.
