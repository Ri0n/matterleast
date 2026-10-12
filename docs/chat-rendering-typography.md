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

Fenced code is rendered in `CodeBlockEdit`, a non-wrapping plain-text editor.
It uses the system fixed font with a typewriter/fixed-pitch hint while matching
the chat point size. Inline Markdown code spans remain in QTextDocument; after
Markdown parsing they must resolve the generic fixed-pitch/monospace family to
the platform's installed system fixed font without stripping other text
formatting, including background and links.

The tests `MessageContentWidgetTest` and `PostAuthorGroupingTest` cover
quote clipping, non-scrolling text, code font and nickname sizing on Qt 5/6.
