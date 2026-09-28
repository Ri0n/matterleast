# User groups

MatterLeast exposes Mattermost user groups from **Sidebar header menu → User groups…**.

The group browser is server-backed. It lists groups through `/api/v4/groups` with member
counts and opens a group detail/editor on activation. Directory-backed groups are intentionally
read-only; only groups whose source is `custom` are edited by the client.

## Custom group mutations

Creation mirrors the official webapp payload:

- `POST /api/v4/groups`
- `source: "custom"`
- `allow_reference: true`
- `name` is the mention without the leading `@`
- `display_name` is the human-readable name
- `user_ids` contains the initial members

Creating a custom group requires at least one member, matching the official UI. Mentions are
lowercase and may contain ASCII letters, digits, `.`, `-` and `_`; Mattermost special mentions
(`all`, `channel`, `here`) are rejected before a request is sent.

Editing stages all UI changes until Save. The commit sequence is:

1. `PUT /api/v4/groups/{id}/patch` for display name / mention;
2. `POST /api/v4/groups/{id}/members` for additions;
3. `DELETE /api/v4/groups/{id}/members` with a JSON `user_ids` body for removals.

This is not server-atomic, so a failure after an earlier successful step is reported rather than
pretending the whole edit rolled back.

## Membership discovery

Existing members are loaded through `/api/v4/users?in_group=...` in pages. The editor's
"Add people" field uses the existing server-backed user search service; search results are
generation-gated so stale responses cannot replace newer text.

## Relationship to @mention autocomplete

Group management and group autocomplete share `MentionGroupService`, but they have different
server authorities. See [Mention autocomplete](mention-autocomplete.md). The group browser must
not replace the dedicated referenceable-group autocomplete search.

## Test data

Tests for this subsystem use synthetic fixture names only. Do not copy real user names, real group
names, organization names, or other production identifiers from screenshots/logs into unit tests.
