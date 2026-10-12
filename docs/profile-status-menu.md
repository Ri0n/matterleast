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
