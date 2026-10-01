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

MatterLeast deliberately uses a compact **20 px** indentation step in the rich composer. `RichTextBlockPresentation.cpp` applies it once at the presentation layer with `document()->setIndentWidth(20.0)`. The logical `QTextListFormat::indent()` remains untouched, so Tab/Backtab semantics and Markdown nesting continue to be structural rather than pixel-based.

`RichTextEditorInteractionTest::listIndentWidthDoesNotChangeMarkdown()` is the cross-version serialization guard: changing the visual indentation width must not change GitHub-flavoured Markdown output.

Do not replace this with per-list margins or marker-padding hacks unless Qt behavior forces it; those would mix layout policy into list structure and make nested-list behavior substantially harder to reason about.

## Code block boundary and language

MatterLeast distinguishes inline code from block code by structural boundaries rather than by the mere presence of a newline inside backticks.

- ordinary one- or two-backtick spans remain inline code;
- the legacy multiline compatibility promotion is used only when the opening delimiter starts a logical line, the closing delimiter ends a logical line, and the span contains at least one newline;
- explicit fenced Markdown (` ```lang ... ``` `) is always a structural code block, including a fence containing only one line of code;
- a non-empty `QTextFormat::BlockCodeLanguage` is structural code-block metadata, never inline-code metadata.

`CodeBlockSupport.h` is the shared source for structural code detection and supported/canonical language names. Empty/default `BlockCodeFence` or `BlockCodeLanguage` Qt properties must not be treated as structural evidence.

When the rich composer cursor is inside a fenced code block, its standard context menu gains **Code language**. **Plain text** clears only `BlockCodeLanguage`; the fenced-code structure stays intact. A named language applies to the complete contiguous code-block region and serializes as the Markdown fence info string.

Renderer parsing normalizes empty code-block properties after Qt Markdown parsing so an inline-code paragraph cannot be materialized accidentally as `CodeBlockEdit`.
