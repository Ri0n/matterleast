# Desktop notification policy

MatterLeast checks desktop notification eligibility in
`MainWindow::messageNotify()`, not in the sidebar presentation. The
`NotificationManager` only delivers notifications already approved by that
policy.

## Channel root messages

Settings → **Notifications** → **Channel message notifications** controls
ordinary new root messages in public/private channels:

| Stored value | Mode | Root post not mentioning the user |
| --- | --- | --- |
| 0 | Mentions only | Silent |
| 1 (default) | Mentions or Favorites | Notify if the channel belongs to the server-side Favorites category |
| 2 | All unmuted channels | Notify |

Explicit mentions notify in every mode. Favorites membership is read through
`SidebarService::isChannelFavorite()` from the full server category membership,
not from currently materialized tree rows.

The global suppressions in `MainWindow::messageNotify()` apply **before**
this policy: the user's own posts and muted channels do not notify.
Notifications for a currently active channel are also suppressed, as before.

## Reconnect replay and notification freshness

Mattermost's reliable WebSocket resume may replay `posted` events after
network recovery, including posts that are days or months old. Such events
still reach the backend for timeline/read-state reconciliation, but they are
**not** new desktop messages. Before displaying a popup, `MainWindow`
checks the original post's server `create_at` (milliseconds since epoch):

- Posts more than **five minutes** old are never shown as desktop popups.
- Missing timestamps and posts dated more than **one minute in the future**
  are also excluded.
- Already materialized post IDs are deduplicated at the WebSocket event
  handler; a repeated `posted` event does not become a new `onNewPost` signal.
  No separate cache of IDs is created for desktop notifications.
- Unexpectedly old WebSocket `posted` events are logged with their post ID,
  channel ID and original timestamp, without logging message bodies, so the
  actual source of historical events can be investigated.

This gate does not discard history, alter server read state, or suppress
unread/Attention synchronization. The timestamp displayed by a desktop
notification service describes when the popup was created, not necessarily
when the underlying Mattermost post was written.

## Unchanged cases

- New DM/GM root messages keep their existing notification behavior, regardless
  of the channel-root setting.
- Thread replies do not consult the channel-root setting or Favorites.
  Replies notify if they mention the user **or** their thread belongs to the
  loaded server-backed Following snapshot. Synthetic root-mention markers
  are not treated as followed threads.
- The global muted-channel and own-post suppressions still apply to replies.
  Unread counters, Attention and read tracking remain separate from desktop
  notification eligibility.
- A notification is suppressed only if its **exact channel or thread**
  is actually visible in a focused top-level window. Merely having the parent
  channel selected, a hidden thread tab, or an unfocused MatterLeast window
  does not suppress a notification. Docked and detached visible threads
  follow the same rule.

The setting is persisted as integer
`notifications/channelRootMode` in `MLOptions` and read for each incoming root
post, so changes made in Settings take effect after pressing **OK**, without
restarting. Unknown persisted values fall back to mode 1.

## Tests

`RootNotificationPolicyTest` covers the three-mode Cartesian matrix of
mention/Favorites states, preservation of DM/GM notifications, and the default.

## Linux desktop identity

The Qt desktop-file name, freedesktop notification icon, and
`desktop-entry` hint all use `io.github.Ri0n.MatterLeast`, matching the
installed `.desktop` file. The previous `mattermost-qt` notification identity
was stale after renaming. Running an uninstalled build from a source checkout
can still log a desktop-portal registration warning because there is no
corresponding installed desktop entry; that warning alone does not establish
that notifications were filtered by the application's policy.

Routine membership join/leave churn is not a desktop notification.
`MainWindow::messageNotify` now checks `shouldShowMainTimelinePost`, excluding
routine membership posts unless they explicitly concern the current user.
This does not change mention policy or muted-channel suppression.

## Taskbar attention and window focus

Incoming notifications never raise or activate MatterLeast windows. Desktop
notifications remain eligible while the user works in another visible window,
including a detached MatterLeast thread. The taskbar/window attention hint is
requested only when the whole application is inactive, and is targeted at the
main window, not at every top-level dialog or detached thread. Clicking an
actual desktop notification still activates MatterLeast and navigates to the
post as an explicit user action.


## System tray unread badges (issue #278)

The tray icon is a presentation of existing read/attention state, not a new
notification trigger and not a second unread counter.

A **single bottom-right badge** is shown, by priority:

1. **Red numeric badge** — `AttentionList` has one or more items requiring
   personal attention. The existing count and its `99+` cap remain intact.
2. **Blue dot** — there is no personal attention, but at least one tracked,
   non-muted public/private channel has unread messages, according to
   `SidebarService::isChannelUnread()`. DMs/GMs, untracked and muted
   channels do not independently trigger the blue dot.
3. **No badge** — neither condition applies.

Red and blue badges are **mutually exclusive**. Both sit at the bottom-right
of the application icon and have a narrow *transparent cutout* around their
rounded shape. The cutout uses `QPainter::CompositionMode_Clear` to remove
alpha from the underlying icon; no tray/panel background color is guessed.
The badge painter supports multiple icon resolutions and device pixel ratios.

`MainWindow` updates the badge when Attention changes or `SidebarService`
reports channel activity/membership, read acknowledgement, or category
changes, including after realtime recovery. Only a change in the badge
state/count causes `QSystemTrayIcon::setIcon()`. Clearing the red badge
immediately reveals the blue dot if another unread ordinary channel remains.
The tray icon resets to the unbadged application icon when the main window
is destroyed, preventing stale state across logins.

Implementation: `ui/TrayBadgePolicy.h`, `ui/TrayBadgePainter.{h,cpp}`,
`ui/IconUtils.cpp`, and `MainWindow::refreshTrayIcon()`.
`ThemeIconWidgetsTest` tests badge priority, eligibility, alpha-cutout,
red/blue exclusivity and high-DPI rendering. This is independent of the
desktop notification preference modes described above.

Build integration: the main application discovers the new
`TrayBadgePainter.cpp` via CMake's recursive source glob. Test targets
compiling `IconUtils.cpp` directly (currently `theme-icon-widgets-test` and
`sidebar-item-delegate-test`) must also link `TrayBadgePainter.cpp`.
Otherwise Qt 5's test executable link fails even though the app builds.
