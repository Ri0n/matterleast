# Rich-text composer

The message composer has two editing presentations over one canonical message format:

- **Markdown mode** edits the Mattermost message source directly.
- **Rich Text mode** edits a `QTextDocument` presentation of that source.

The canonical value sent to Mattermost and stored in drafts is always Markdown. HTML and Qt's internal document format are presentation details and must not cross the composer boundary.

## Source preservation

Switching from Markdown to Rich Text parses the current Markdown with Qt's GitHub-flavoured Markdown support. The editor also keeps the exact source string that produced the rich document.

Until the rich document is actually modified, `markdownText()` returns that original source verbatim rather than asking Qt to serialize it again. This matters for Mattermost/raw Markdown constructs that Qt does not understand: merely viewing them in Rich Text mode must not normalize or destroy them.

After a real rich edit, the document is serialized back with `QTextDocument::MarkdownDialectGitHub`. Switching back to Markdown mode exposes that serialization for direct inspection/editing.

## Integration contract

`MessageTextEditWidget::markdownText()` / `setMarkdownText()` are the semantic boundary. The historical `toPlainText()` / `setPlainText()` names are intentionally hidden by `MessageTextEditWidget` and route through that Markdown-aware API so existing `OutgoingPostCreator` send, edit, and draft code keeps working without acquiring rich-text semantics itself.

Mentions and emoji remain ordinary inserted text. Attachments remain outside the text document. The rich-mode toggle is an explicit user action; Markdown mode remains the default and predictable escape hatch.

## Formatting commands

The composer exposes commands for bold, italic, strike-through, inline code, code blocks, links, quotes, bulleted lists, and numbered lists. In Markdown mode these commands edit Markdown syntax directly. In Rich Text mode they change the `QTextDocument` formatting and mark the source snapshot dirty so subsequent serialization reflects the edit.

Conventional keyboard shortcuts are handled at the editor layer where practical (`Ctrl/Cmd+B`, `Ctrl/Cmd+I`, `Ctrl/Cmd+K`, and `Ctrl/Cmd+Shift+X`). Completion-popup navigation keeps priority while the popup is open.
