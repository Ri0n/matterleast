# Virtual sidebar destinations and post collections

This note records the model for user-centric destinations that look like navigation entries but are
not ordinary server sidebar rows, plus the common collection semantics needed by Saved, Recent
Mentions, and message search.

## Personal

The user's self-contact/self-DM is exposed as **Personal** (`Личное`) as the first local row inside the
existing **Favorites** sidebar category.

`Personal` is a virtual navigation item, but its destination is a real canonical self-DM channel. It
resolves the logged-in user's direct channel with themselves and opens the ordinary channel/timeline
path. If that direct channel has never existed, activation creates it through Mattermost's normal
`/channels/direct` path first; there is still no fake `BackendChannel`.

Consequences:

- `Personal` has a stable local destination identity independent of a server category row;
- the real self-DM keeps the normal channel ID, post cache namespace, unread/history/thread behavior;
- the virtual row is non-draggable and never participates in sidebar category mutation payloads;
- a real self-DM row is suppressed inside Favorites so the same conversation is not shown twice there;
- if the server later exposes the self-DM as an ordinary DM row elsewhere, activating it redirects to
  `Personal` as the user-facing canonical shortcut;
- the row can use the logged-in user's avatar while keeping the label `Personal`.

`SidebarItem::VirtualDestination` and `SidebarItem::DestinationRole` keep this semantic distinction
explicit instead of pretending the local row is an ordinary server channel row.

## Saved

**Saved** (`Сохранённое`) is fundamentally different. Saved posts can originate from multiple channels
and threads, so it does not pretend to be a `BackendChannel`.

It is implemented as the second fixed local row in Favorites and opens the shared virtualized
`PostCollectionView`. The producer is the paged `/users/{user_id}/posts/flagged` endpoint. Ordinary
message context menus can add `flagged_post` preferences and Saved rows can remove them again. The
sidebar row is a concrete virtual-destination item with no channel context menu: it deliberately cannot
inherit mute, profile, or category-mutation actions from an unrelated real channel.

The destination is backed by a cross-conversation post collection:

```text
Saved
  +-- post A from channel X
  +-- post B from thread Y / root R
  +-- post C from channel Z
```

Each collection entry keeps semantic origin, not a fabricated conversation coordinate:

```text
postId
channelId
rootId      optional; non-empty means the origin is a thread
```

Opening an entry resolves the real channel/thread and then performs ordinary semantic post-ID
navigation. The collection itself never invents channel page numbers or thread cursor adjacency.

## Recent Mentions

**Recent Mentions** is another fixed virtual destination inside Favorites, after Personal, Saved, and
Drafts. Like Saved, it is a cross-conversation collection rather than a fake channel. It reuses
`PostCollectionView`, collection paging, origin labels, and canonical `AppNavigationService::openPost()`
activation.

The producer intentionally follows Mattermost web-client mention-search semantics instead of deriving a
second client-side mention model:

- start with the logged-in user's configured personal mention keys;
- include the first name only when `notify_props.first_name` is enabled and always include `@username`;
- exclude broadcast keys `@channel`, `@all`, and `@here`;
- quote each remaining key before search, so keys containing dashes or other search syntax stay atomic;
- issue an all-team `posts/search` request with `is_or_search=true` and `include_deleted_channels=true`;
- preserve the search endpoint's result order and paging authority.

`RecentMentionsService` owns only the feature policy (deriving the current user's query). The actual
`posts/search` transport and collection-response normalization remain under `PostRepository`, preserving
its repository-wide ownership of post REST retrieval.

Mattermost commonly serializes notification booleans in `notify_props` as the strings `"true"` and
`"false"`. `BackendNotifyPreps` therefore accepts both those strings and JSON booleans. Recent Mentions
must use that normalized user state rather than reinterpreting the raw profile JSON independently.

Recent Mentions does **not** inherit Attention semantics. Opening the collection does not mutate unread
state, follow state, or mention counters; selecting a result navigates to the canonical conversation,
where the normal read-tracking rules take over.

The current sidebar placement deliberately remains inside Favorites. Moving Personal/Saved/Drafts/
Recent Mentions above server categories is a separate presentation decision and must not be coupled to
the collection model.

## Message search

Message search reuses the same collection/navigation model as Saved and Recent Mentions. The difference
is lifetime and producer, not row semantics. A magnifier beside the sidebar menu opens the transient
Search page; queries are sent to Mattermost's search endpoint with the server-side search syntax kept
authoritative. The UI exposes the standard modifiers `from:`, `in:`, `before:`, `after:` and `on:`, plus
reminders for quoted phrases, exclusions, suffix wildcards and hashtags. Search can target the
current/specific team or the server's all-team search endpoint when supported.

```text
Saved / Recent Mentions             Search result collection
fixed virtual destination           ephemeral query result set
          |                                    |
          +------------- common entry ----------+
                        postId
                        channelId
                        optional rootId
                              |
                              v
                  canonical conversation
                              |
                        navigate to post
```

This means collection results should not be inserted into `BackendChannel::posts` as if they formed a
contiguous history window. A collection/search endpoint proves only that those posts belong to the
result and their result ordering; it does not prove adjacency in the source conversation.

The shared collection layer therefore owns:

- ordered collection entries and collection-specific paging;
- lazy body resolution through `PostRepository::loadPost()` where needed;
- origin labels/context preview;
- activation into channel versus thread based on `rootId`;
- semantic `goToPost(postId)` after the real conversation is open.

`Saved` and `Recent Mentions` are represented by fixed virtual navigation destinations. Search results
are normally a transient destination created by a search action, but all reuse the same post-collection
view machinery.

### User-driven paging

A collection exposes at most ten new rows for each paging step. Loading the first search page does not
create an unavailable sentinel and does not let `LongListWidget` prefetch trigger another search. Only
a direct user viewport gesture near the bottom asks the collection for the next ten rows.

Mattermost installations do not all implement search pagination identically. In particular, some search
backends can ignore the requested `page`/`per_page` and return a larger bounded result set. The
repository marks such a response as a complete result snapshot. `PostCollectionView` keeps that raw
snapshot buffered, materializes the first ten posts, and reveals the next ten only as the user scrolls.
No duplicate server request is made for those buffered pages. When the server honors pagination, each
user paging gesture requests the next server page normally.

This separation is intentional:

```text
server response                    collection-visible rows
      |                                      |
      | <= 10, has next page                 +-- first 10
      +-------------------------------------->+-- user scroll -> request next page
      |
      | > requested page size                +-- first 10
      +--> buffered complete snapshot ------>+-- user scroll -> reveal buffered 10
                                             +-- no repeated search request
```

### Interactive search input

Search uses the same `InteractiveTextEdit` foundation as the message composer. The widget itself owns
only generic editor/completion mechanics; each use case installs its own completion rules and providers.
A rule defines a trigger prefix, candidate provider, human-facing display text, canonical insertion text,
and additional filter keys.

For search the initial rules are:

- `in:` — candidates are known channels. Matching is case-insensitive `contains` over the displayed
  channel title and, when human-readable, the canonical channel name/slug. Selecting a normal
  public/private channel replaces the typed value after `in:` with its canonical channel name; DM/group
  rows fall back to the channel ID where needed by server search semantics.
- `from:` — candidates are known users. Matching includes display name, username, nickname, first name,
  and last name; selection inserts the canonical username.

The popup opens as soon as a configured prefix is active. Continuing to type filters the existing
candidate set. Selection replaces the complete value belonging to that prefix up to the next whitespace,
so editing in the middle of an existing token cannot leave stale suffix text behind. A leading exclusion
marker is preserved, therefore `-in:` and `-from:` reuse the same rules.

`CompletionCandidate::displayText` and `insertText` are deliberately separate. The first implementation
keeps the editor/query wire representation plain text because Mattermost search syntax is textual. This
also leaves room for a future rich visual token/atom presentation without changing completion providers
or the canonical value sent to the server.

The composer currently inherits the same editor foundation while preserving its existing Enter,
Shift+Enter, Escape, previous-message edit, and auto-height behavior. Composer-specific completion rules
can be added independently of the search rules.

## Cache interaction

The persistent post cache may make Saved/Recent Mentions/Search rows paint quickly because collection
entries identify individual posts. It still receives no extra timeline authority from those collections.
A cached body can satisfy first paint and normal HTTP validation can refresh it, while channel/thread
sources remain the only owners of conversation placement.
