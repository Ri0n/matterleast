# Profile status actions

Clicking the avatar opens a standard menu for Mattermost availability:
online, away, do not disturb and offline. A separate editor supports
custom status text (up to 100 characters), an emoji selected using the
existing emoji picker, and an expiry duration (30 min, 1/4 hours, today,
this week, or an explicit date/time). Clear custom status and View profile
remain available in the same menu.

The BackendOwnStatus implementation uses Mattermost REST API:
`PUT /users/{user_id}/status`, `PUT /users/{user_id}/status/custom` and
`DELETE /users/{user_id}/status/custom`. It commits new values locally
only after a successful HTTP status; failed updates leave the visible state
unchanged and display the error. Presence is shown by the avatar badge;
the custom status message appears under the account name.

These actions are independent of team selection, which is rendered only
inside the Channels tab.

## Empty editor semantics

**OK always commits the user's intent.** Emoji-only sends `PUT` with a
non-empty emoji and empty text. Text-only also remains valid. When both
fields are empty, OK closes the dialog and issues `DELETE` to clear the
custom status. Cancel leaves the previous status untouched. The separate
Clear custom status menu action stays available, including for emoji-only
statuses. The menu/header recognize a status as active when either emoji
or text is set. The existing emoji registry renders it in the header.

## Compact sidebar presentation

The navigation header keeps the custom status label visible even though the
account name is hidden; ordinary availability remains on the avatar badge.
When custom status has an emoji and no non-whitespace message, the emoji is
rendered at **three times its standard size**. Unicode glyphs use the scaled
emoji font; custom emoji images use a 48px extent rather than the regular
16px reaction-size presentation. Mixed emoji + message and text-only statuses
retain the original font and icon size. Changes in custom status always reset
from the label's original font so switching statuses never compounds scaling.
