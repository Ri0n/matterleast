# Sidebar Unread filtering

There are two independent Unread controls. Neither creates a new unread model
nor marks messages read.

## Unread tab (issue #277)

The second left-sidebar tab replaces **Following** with **Unread**. It has
three compact, independently persisted checkboxes directly above the list:

| Checkbox | Default | Meaning |
| --- | --- | --- |
| **Following** | On | Existing `FollowingModel` queue, including unread DMs/GMs, explicitly followed threads even when read, and synthetic root mentions. Its sorting, thread navigation, read/resume cursors and selection retention remain unchanged. |
| **Channels** | On | Additional public/private channels that are joined/tracked and currently unread according to `SidebarService`, **including channels with outstanding personal mentions**. No new persistent Following membership or read cursor is created. DMs/GMs are not introduced by this source. |
| **Muted** | Off | An independent source of unread muted conversations (including DMs/GMs), even with Channels unchecked. The old explicit-followed-thread exception remains unchanged. |

The checkboxes are stored in `MLOptions` under
`sidebar/unreadView/{following,channels,muted}`, with defaults `true/true/false`.
Changes immediately rebuild the view and clear any obsolete selection-retention
snapshot; they do not change the Attention tab or the server's mute/read state.
For example, Following on + Channels off + Muted on shows the old Following
queue together with unread muted conversations, without ordinary unmuted
channels.

The projection is a union with **three priority groups**, top to bottom:

1. **Unread Following** — unread DMs/GMs, followed threads and synthetic
   mentions from the shared Following model. This group always comes before
   ordinary unread channels, regardless of relative timestamps.
2. **Unread Channels + Muted** — one mixed chronological stream of unread
   channel-only rows (including the independent Muted source). A newer muted
   channel sorts above an older unmuted channel, and vice versa.
3. **Read Following** — already-read followed threads and other read items
   still owned by Following, after every unread item.

Within a group, sorting is descending by activity timestamp. Following
unread uses `attentionSince` when available, otherwise `lastReplyAt`;
read Following uses `lastReplyAt`; channel-only and muted entries use
`SidebarService::channelActivityTime()`. Equal timestamps break ties by the
stable semantic key, and unknown timestamps sort last within their group.

Stable identity is `c:<channelId>` for conversations and `t:<threadId>`
for threads. When an unread channel is already represented by Following
(for example, an explicit Mark as unread), it appears **once**; the
model-backed Following entry wins and retains its existing resume semantics.
A personal mention may instead be represented by a Following post/thread row,
not by the channel's `c:<channelId>` identity. That separate row does not
replace the ordinary unread channel: it stays visible while `SidebarService`
still reports unread posts, even if the mention is opened or acknowledged.
Neither tab computes its own unread state.
A selected Following conversation may remain momentarily after being read
for click stability, but it belongs to the **read** group, never to the
unread Following or Channels groups. A read Channels/Muted-only row cannot
masquerade as read Following through selection retention.

Clicking a Channels-only row asks the server for its unread post and navigates
to the returned post (falling back to the channel when the server cannot supply
one). Unlike a Following row, this view-only entry does not establish a
`FollowingModel` resume cursor. Middle-click opens its channel in a new tab.
Selection retention is ephemeral and never means a post was read. Filtering
by text continues to apply to the list.

## Icon semantics shared with Channels

Channel icons are independent of the tab or source that contributed the row:
public channels and group conversations use the people icon, private channels
use the lock icon, followed threads use the hash (`#`), and DMs use the
participant's avatar. The icon policy lives in `ChannelIcons::conversation()`
and `ChannelIcons::thread()` and applies to initial materialization, fallback
painting and palette refresh. Do not switch icons between Channels and Unread.

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
