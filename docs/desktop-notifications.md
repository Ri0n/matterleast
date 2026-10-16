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
