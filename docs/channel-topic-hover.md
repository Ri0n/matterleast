# Channel topic hover

The one-line `ChannelHeaderTextLabel` expands multiline/overflowed
Markdown in `channelHeaderTextPopover` without resizing the chat timeline.
The overlay is still animated and stays within the ChatArea host.

Unlike an instant popup, opening requires a tooltip-style mouse dwell (style
hint `QStyle::SH_ToolTip_WakeUpDelay`, clamped to 250–700 ms). Start the
single-shot show timer only on the collapsed label's Enter. Cancel it on
Leave, hiding the label, or clearing the topic. This prevents a brief pointer
crossing from opening an overlay.

Once the overlay is visible, crossing between label and overlay must not
restart the delay or destroy its rich text children. Re-entering during a hide
animation should reverse it immediately. Existing hide-delay and animation
behavior remain unchanged. Direct `showPopover` calls are kept for GUI tests.

## Channel and user references

The collapsed topic QLabel and the expanded MessageContentWidget use the
same `UserMentionLinkifier::linkify()` as chat posts. No topic-specific regex
or channel-name guessing is needed: `~channel-slug` becomes a team-scoped
`/<team>/channels/<slug>` link, while the existing linkifier preserves inline
code and explicit Markdown links. The team slug is resolved from the current
ChatArea's channel team, with the same current-team fallback as conversation
URLs.

The expanded topic reconnects the shared linkifier and its navigation handlers
on every `MessageContentWidget::contentRebuilt` signal; palette, emoji and
content rebuilds must not lose clickability. Relative channel links use
`AppNavigationService`; user mentions use `UserProfileService`. Existing URL
tooltips and hover expansion remain unchanged.
