# Rich-text composer follow-ups

Focused implementation follow-ups that should not be lost while the interaction model is being built out.

## List send/serialization regression

Reproduce and keep covered by tests:

1. start with an empty rich-text composer;
2. enable a bullet or numbered list from the toolbar;
3. type multiple items using Enter;
4. send the message;
5. verify the wire/draft Markdown contains real list markers rather than plain multiline text.

`RichTextEditorInteractionTest::listStartedInEmptyComposerSerializesAsMarkdownList()` covers the document -> Markdown boundary. Temporary `RICH_TEXT_LIST` diagnostics print list object/style/indent state plus Qt's immediate `toMarkdown()` output after structural list commands. If the document-level test is green but a real send is still plain text, trace the next boundary in `OutgoingPostCreator::sendPostButtonAction()` / outbox staging rather than changing list editing semantics.

## List visual indentation

Qt separates list nesting from its on-screen indentation. `QTextListFormat::indent()` stores the logical indentation level while `QTextDocument::indentWidth()` controls the pixel width of one indentation step; Qt's default is 40 px.

The composer currently inherits that Qt default, which makes top-level lists look too far from the left edge in the compact message editor.

Before changing the presentation value:

- run the Qt 5.15 and Qt 6 interaction tests proving that changing `QTextDocument::indentWidth()` does not alter GitHub-flavoured Markdown serialization;
- choose a compact value visually appropriate for the composer (start by evaluating ~20 px);
- verify nested lists still have clearly distinguishable levels;
- keep `QTextListFormat::indent()` untouched so keyboard Tab/Backtab semantics and Markdown nesting remain structural rather than pixel-based.

Once the cross-version test is green, the likely implementation is one composer-level `document()->setIndentWidth(...)` setting rather than per-list margins or format hacks.
