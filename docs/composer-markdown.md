# Markdown composer

MatterLeast edits message text as raw Mattermost Markdown. The composer does not maintain a second rich-text/WYSIWYG document and does not serialize through `QTextDocument::toMarkdown()`.

The text visible in `MessageTextEditWidget` is the same text used by drafts, post editing, retries, and post creation. Formatting controls are helpers that insert or wrap Markdown syntax; they do not introduce a second representation.

## Formatting toolbar

The toolbar appears above the composer while the editor has focus. Its persistent *show on focus* preference is stored in `composer/formattingToolbarVisible` through `MLOptions`.

- `Ctrl/Cmd+Up` enables the toolbar preference and shows it.
- `Ctrl/Cmd+Down` disables the preference and hides it.
- Losing focus hides the current toolbar without changing the preference.
- The editor context menu exposes **Show formatting toolbar** / **Hide formatting toolbar**.

The toolbar inserts these Markdown primitives:

- bold: `**text**`
- italic: `_text_`
- strikethrough: `~~text~~`
- inline code: `` `text` ``
- fenced code block: triple-backtick fences around the selection
- quote: `> ` at the beginning of selected/current lines
- bulleted list: `- ` at the beginning of selected/current lines
- numbered list: sequential `1. `, `2. `, ... prefixes
- link: `[label](https://...)`

Conventional shortcuts are handled at the editor layer where practical: `Ctrl/Cmd+B`, `Ctrl/Cmd+I`, `Ctrl/Cmd+K`, and `Ctrl/Cmd+Shift+X`. Completion-popup navigation keeps priority over ordinary editor navigation.

The toolbar slot expands/collapses over 110 ms using `OutCubic` / `InCubic`, matching the interaction style used by other compact composer controls.

## Links and paste

The editor uses the standard Qt text-editor context menu. When the cursor or selection intersects a Markdown link, the menu additionally offers **Edit link…** and **Remove link**.

Pasting a single HTTP(S) URL while ordinary single-line text is selected turns the selection into `[label](url)` rather than replacing the label. Image paste remains owned by `OutgoingPostCreator` and continues to create an attachment.

## Message priority

Priority is independent of Markdown. Mattermost expects it in top-level `metadata.priority`, so it must never be encoded into message text or `props`.

The composer supports Standard, Important, and Urgent. Important/Urgent can request acknowledgement; Urgent can also enable persistent notifications. Priority is available only for new root posts.

Priority metadata is staged by `pending_post_id` in `PostCreateService`, so retries keep the same metadata and successful/terminal delivery clears it together with the pending request state.

## Rendering boundary

This feature must not change how received messages are rendered. `MessageFormatter` and `MessageContentWidget` are independent of the composer formatting toolbar. Changes to raw Markdown editing should therefore not require message-log rendering changes.
