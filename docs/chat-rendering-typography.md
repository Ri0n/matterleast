# Chat rendering typography and quote behavior

The Appearance settings have separate interface and chat font sizes. The
interface font (`font.interface`, empty means application default) is applied through
QApplication before windows are created and updated live. Cancel restores the
previous setting. `FontSettingsWidget` takes `{id, label, font}` entries and emits
serialized QFont changes by ID; the settings dialog binds these to `font.<id>`.
Each grid row contains a label, family selector, size and short slider. Chat
uses `font.chat`; startup migrates legacy `chat/font` and `ui/fontPointSize`
values only when their new keys are absent. Both fonts default to 11 pt with
the platform's font family. The chat default is registered before applying
interface overrides, so a customized interface size cannot become chat's default.
Saved font choices, including migrated values, take precedence over defaults.
Sidebar categories inherit the interface font,
and conversation rows and team buttons grow to fit its metrics rather than
clipping enlarged text into fixed heights.

The timeline is the only vertical scroll owner for chat messages. Ordinary
Markdown `>` blockquotes reuse `QuotedPostPreview`'s measured excerpt
and Show more/less implementation with no author header or click-to-navigate
callback. They start at four visual lines; Show more appears only when the
rendered document exceeds that height, and Show less restores the compact
view without losing any trailing words. Referenced-post previews share the
same measurement and controls but default to two lines (four for embedded
permalinks), as described in `quoted-replies.md`.

Blank source lines separating successive quoted paragraphs remain inside
one quote widget so the left border is continuous, including when expanded.
Non-quoted prose still separates independent quote blocks. The inner text
browser stays scrolled to position zero and ignores wheel events so the
containing chat log remains the only vertical scroll owner. Do not switch
the nested quote to a scrollable text area; it would trap wheel events and
interrupt timeline navigation. Ordinary Markdown quote text remains
selectable, and hyperlinks continue to work independently of expansion.

The author label in `PostWidget` uses `CHAT_FONT` at the same point size as
the message body with bold weight. Do not reintroduce Designer's old 8 pt /
16 px maximum-height constraints, which clip larger configured chat fonts.
Timestamps intentionally use a smaller font derived from the author label.

Timestamp presentation follows the surface: channel/DM/GM posts show a
locale-formatted clock, while thread posts retain relative timestamps.
All conversation timelines, including threads, show a visually distinct
local-calendar date divider at the beginning of each day.
The avatar/text gap matches the row's left avatar inset rather than inheriting
the zero spacing of the outer post/day-separator layout.
The engagement row immediately beneath message content has a 3px top inset
whenever it displays a thread summary or emoji reactions (or both), and no
extra inset when it has neither. Thread-only posts must retain the same visual
separation as posts with emoji reactions.
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

Markdown hyperlinks have a hover tooltip containing their actual target,
including links whose visible labels differ from their URLs. The shared
MessageFormatter attaches character-format tooltips for QTextDocument
consumers; its HTML export additionally injects anchor title attributes
because Qt's exporter drops QTextFormat::TextToolTip. This covers posts,
blockquotes, quote previews, and collapsed/expanded channel topics.
Non-link text has no link tooltip. Quote preview full-text fallback tooltips
belong to the surrounding frame, not the inner text browser.
