# Chat rendering typography and quote behavior

The timeline is the only vertical scroll owner for chat messages. Ordinary
Markdown `>` blockquotes are displayed through `QuoteBlock` and
`WrappedRichText` in `MessageContentWidget.cpp`. Quote previews show up to
four line-heights, clip the remaining document, and stay scrolled to position
zero. Their wheel events must be ignored by the nested text editor so the
containing chat log can continue scrolling. Do not switch the nested quote to a
scrollable text area; it would trap wheel events and interrupt timeline navigation.

The author label in `PostWidget` uses `CHAT_FONT` at the same point size as
the message body with bold weight. Do not reintroduce Designer's old 8 pt /
16 px maximum-height constraints, which clip larger configured chat fonts.
Timestamps intentionally use a smaller font derived from the author label.

Timestamp presentation follows the surface: channel/DM/GM posts show a
locale-formatted clock and a visually distinct local-calendar date divider at
the beginning of each day; thread posts retain relative timestamps.
Search, Saved, Drafts, Recent Mentions and Pinned collections display a
full system-locale date *and* time for every post, without requiring hover.
A date divider belongs to the presentation of the first post in a day,
not to the source model: it must not introduce an extra row index, phantom
post ID, unread cursor or navigation anchor. On sparse source paging, only
show a boundary when its preceding post is known (or the row is index zero).
Recalculate dividers whenever author grouping is recalculated.

Fenced code is rendered in `CodeBlockEdit`, a non-wrapping plain-text editor.
It uses the system fixed font with a typewriter/fixed-pitch hint while matching
the chat point size. Inline Markdown code spans remain in QTextDocument; after
Markdown parsing they must resolve the generic fixed-pitch/monospace family to
the platform's installed system fixed font without stripping other text
formatting, including background and links.

The tests `MessageContentWidgetTest` and `PostAuthorGroupingTest` cover
quote clipping, non-scrolling text, code font and nickname sizing on Qt 5/6.
