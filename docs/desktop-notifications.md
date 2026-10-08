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
- Thread replies never consult the channel-root setting or Favorites.
  The existing mention-driven desktop-notification path is unchanged.
- Following/Attention membership, unread counters and read tracking are
  separate from desktop notification eligibility. Do not infer notification
  preferences from Following UI state.

The setting is persisted as integer
`notifications/channelRootMode` in `MLOptions` and read for each incoming root
post, so changes made in Settings take effect after pressing **OK**, without
restarting. Unknown persisted values fall back to mode 1.

## Tests

`RootNotificationPolicyTest` covers the three-mode Cartesian matrix of
mention/Favorites states, preservation of DM/GM notifications, and the default.
