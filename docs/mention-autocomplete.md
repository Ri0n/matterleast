# Mention and channel-reference autocomplete

Composer `@` completion has two independent server authorities:

- users come from the bounded Mattermost `/users/autocomplete` endpoint, with a
  small channel-member set used only for immediate local suggestions;
- user groups come from the referenceable-group search endpoint
  `/groups?q=...&filter_allow_reference=true`.

Do not treat `/teams/{team_id}/groups` as the autocomplete authority. That
endpoint is useful as a local cache of team-associated groups, but it can omit
referenceable custom groups which the official webapp discovers through the
global group search.

## Composer `@` contract

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

## Composer `~` contract

Channel references use a separate `InteractiveTextEdit::CompletionRule` with
`~` as the trigger. A bare `~` is answered synchronously from the team's joined
channels, so opening the popup never waits for the network. Once the user types
a non-empty prefix, `Backend::searchTeamPublicChannels()` also searches the
server-side public-channel directory. This allows unjoined public channels to
be referenced without preloading or opening Browse Channels first.

The local and remote lists are deduplicated by canonical channel name. The
inserted token is the URL-safe channel `name`; `display_name` is presentation
only. Purpose text participates in filtering but is not inserted into the
message.

As with group mentions, DM and GM composers use the current UI team context
because direct channels do not carry an intrinsic `BackendTeam`.

## Rendered channel references

Rendered message text recognizes `~channel-name` outside existing anchors and
code ranges. The reference is converted to the normal Mattermost team route
`/<team>/channels/<channel-name>`, so clicks continue through
`AppNavigationService` and inherit ordinary channel navigation semantics:
existing destinations are reused, tab duplicate prevention remains active, and
unknown/non-resident targets retain the existing browser fallback.

The renderer derives the team from the owning `ChatArea`; DM/GM views use the
same current UI team-context fallback as composer completion.

## Cache role

`MentionGroupService::ensureTeamGroups()` is a warm local cache for rendering,
linkification and immediate suggestions. A successful dynamic group search is
also folded into that team-scoped cache when a team context exists.

The cache is not allowed to suppress server group discovery. This distinction
is important for custom groups and for installations whose group associations
do not mirror the set of referenceable mentions visible to ordinary users.
