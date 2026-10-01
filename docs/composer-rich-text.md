# Rich-text composer

The message composer has two editing presentations over one canonical message format:

- **Rich Text mode** edits a `QTextDocument` presentation of the message and is the normal ChatArea composer presentation.
- **Markdown mode** edits the Mattermost message source directly and remains available from the editor context menu as the predictable escape hatch.

The canonical value sent to Mattermost and stored in drafts is always Markdown. HTML and Qt's internal document format are presentation details and must not cross the composer boundary.

## Source preservation

Switching from Markdown to Rich Text parses the current Markdown with Qt's GitHub-flavoured Markdown support. The editor also keeps the exact source string that produced the rich document.

Until the rich document is actually modified, `markdownText()` returns that original source verbatim rather than asking Qt to serialize it again. This matters for Mattermost/raw Markdown constructs that Qt does not understand: merely viewing them in Rich Text mode must not normalize or destroy them.

After a real rich edit, the document is serialized back with `QTextDocument::MarkdownDialectGitHub`. Switching to Markdown mode exposes that serialization for direct inspection/editing.

Editing an existing post deliberately uses a hybrid source presentation instead of full Rich Text mode. The complete Markdown source stays visible and is the actual editor document, including code fences, emphasis markers, lists and unsupported syntax. Markdown links are the only decorated construct: their human-readable label is rendered with the current palette's conventional link color and underline while the surrounding `[label](url)` source remains visible and editable. This keeps edit/save byte semantics predictable without making links visually indistinguishable from ordinary source text.

## Integration contract

`MessageTextEditWidget::markdownText()` / `setMarkdownText()` are the semantic boundary. The historical `toPlainText()` / `setPlainText()` names are intentionally hidden by `MessageTextEditWidget` and route through that Markdown-aware API so existing `OutgoingPostCreator` send, edit, and draft code keeps working without acquiring rich-text semantics itself.

Mentions and emoji remain ordinary inserted text. Attachments remain outside the text document.

## Formatting toolbar

The ordinary composer layout is intentionally unchanged: attachment action on the left, editor in the middle, emoji/send actions on the right, and no enclosing composer frame. The formatting toolbar is an auxiliary row inside the editor column and appears above the editor only while the editor owns focus.

Toolbar visibility has a persistent *show on focus* preference (`composer/formattingToolbarVisible`). `Ctrl/Cmd+Up` enables it and shows the toolbar immediately; `Ctrl/Cmd+Down` disables it and hides the toolbar. Losing editor focus always hides the current toolbar instance without changing that preference. The editor context menu exposes the same state as a dynamic **Show formatting toolbar** / **Hide formatting toolbar** action.

The toolbar exposes bold, italic, strike-through, inline code, code blocks, links, quotes, bulleted lists, numbered lists, and message priority. In Markdown mode formatting commands edit Markdown syntax directly. In Rich Text mode they change the `QTextDocument` formatting and mark the source snapshot dirty so subsequent serialization reflects the edit.

The formatting toolbar expands and collapses by animating its real layout height with the same 240 ms `OutCubic` motion profile used by the expanded emoji/reaction picker, including continuation from an interrupted animation state.

Conventional keyboard shortcuts are handled at the editor layer where practical (`Ctrl/Cmd+B`, `Ctrl/Cmd+I`, `Ctrl/Cmd+K`, and `Ctrl/Cmd+Shift+X`). Completion-popup navigation keeps priority over ordinary navigation keys.

## Links and paste

The editor uses the normal Qt text-editor context menu (undo/redo/cut/copy/paste/etc.). When the cursor or selection is on a link, the menu additionally offers **Edit link…** and **Remove link**.

Pasting a single HTTP(S) URL while ordinary non-link text is selected preserves the selected label and turns it into a link instead of replacing it. Image paste remains owned by `OutgoingPostCreatorClipboard` and continues to create an attachment.

In Rich Text mode, pressing Space twice immediately after link-formatted text converts the first inherited linked space into one ordinary space and consumes the second keypress. The same gesture after a Markdown link in source mode collapses the two spaces to one, giving a consistent way to continue typing outside a link.

## Message priority

Priority is a create-post concern, not Markdown. Mattermost expects it in top-level `metadata.priority`, so it must never be encoded into message text or `props`.

The composer supports Standard, Important, and Urgent. Important/Urgent can request acknowledgement; Urgent can also enable persistent notifications and defaults acknowledgement on when newly selected. Mattermost defines priority for new root posts, so thread/edit/send-in-progress composers do not stage priority metadata.

Priority metadata is keyed by the same `pending_post_id` used by the session outbox. `PostCreateService` retains staged metadata across retryable HTTP failures and removes it after successful or terminal delivery, leaving the outbox FIFO/retry model unchanged.
