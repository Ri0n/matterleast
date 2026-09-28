# Mention autocomplete

Composer `@` completion has two independent server authorities:

- users come from the bounded Mattermost `/users/autocomplete` endpoint, with a
  small channel-member set used only for immediate local suggestions;
- user groups come from the referenceable-group search endpoint
  `/groups?q=...&filter_allow_reference=true`.

Do not treat `/teams/{team_id}/groups` as the autocomplete authority. That
endpoint is useful as a local cache of team-associated groups, but it can omit
referenceable custom groups which the official webapp discovers through the
global group search.

## Composer contract

On every non-empty `@` prefix, user and group searches run independently.
Each response is guarded by the same query/generation so an older asynchronous
response cannot repopulate suggestions after the user has typed further or
closed completion.

The candidate provider merges, in order:

1. special mentions (`@channel`, `@all`, `@here`);
2. cached team-associated group mentions;
3. current server group-search results;
4. bounded local channel users;
5. server user-autocomplete results.

Candidates are deduplicated by their inserted mention token.

DM and GM channels have no intrinsic `BackendTeam`. Group lookup therefore
uses the current UI team context for those conversations. User autocomplete
does **not** inherit that fallback: it keeps its existing direct-channel scope
and must not send a fabricated `in_team` + `in_channel` pair for DM/GM.

## Cache role

`MentionGroupService::ensureTeamGroups()` is a warm local cache for rendering,
linkification and immediate suggestions. A successful dynamic group search is
also folded into that team-scoped cache when a team context exists.

The cache is not allowed to suppress server group discovery. This distinction
is important for custom groups and for installations whose group associations
do not mirror the set of referenceable mentions visible to ordinary users.
