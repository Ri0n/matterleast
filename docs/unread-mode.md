# Sidebar Unread mode

The sidebar's **Unread mode** is a presentation filter. It never changes Mattermost read state and it does not create a second unread model; authoritative channel unread/mute state remains owned by `SidebarService`.

## Settings

Two independent settings live under **Settings -> Sidebar -> Unread mode**:

- **Channels only** — when enabled, the Unread toggle filters channel/category/team rows but leaves the **Following** tab available. When disabled, enabling Unread mode also hides Following, preserving the historical MatterLeast behavior.
- **Ignore while filtering** — when enabled (the default), entering text in the sidebar filter temporarily suspends the unread gate for the Channels tree so matching read channels can be found. Clearing the text immediately restores the unread gate. When disabled, text and unread predicates are intersected.

Both settings are `MLOptions` values and take effect immediately after Settings are applied; no restart is required.

## Filtering contract

Conceptually, channel-row visibility is:

```text
matchesText && matchesUnread
```

where `matchesUnread` is only evaluated when the unread gate is active. With **Ignore while filtering** enabled, a non-empty text query disables that gate but does not uncheck the Unread toolbar button; the previous Unread mode resumes when the query becomes empty.

The currently selected channel may remain visible while Unread mode is active even after its server unread state clears. This existing presentation retention prevents the active row from disappearing under the user and is independent of the two policy settings above.

Following remains backed by the complete `FollowingModel` projection, including already-read followed threads. Hiding or showing its tab is therefore presentation policy only and must not mutate Following membership or read state.
