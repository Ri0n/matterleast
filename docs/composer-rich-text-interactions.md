# Rich-text composer interaction model

This document defines the **editing semantics** of MatterLeast's rich-text composer. It is intentionally more specific than the formatting-toolbar description in `composer-rich-text.md`: toolbar buttons are only one way to invoke editor commands. Keyboard input, context menus and structural navigation must produce the same document transitions.

The interaction model is adapted from the mature structural editing rules used by AnyKeep, but the MatterLeast implementation operates on one `QTextDocument` and serializes back to canonical Mattermost Markdown.

## Core invariants

1. **Markdown remains canonical.** Rich text is an editing presentation. Every structural operation must round-trip through `MessageTextEditWidget::markdownText()` without silently changing unrelated source.
2. **Toolbar actions invoke semantic commands.** A button must not merely flip a `QTextCharFormat`/`QTextBlockFormat` property. It invokes the same command that keyboard/context-menu interaction uses.
3. **Structural editing wins over implicit send.** An unmodified Enter is offered to the active structural element before the composer considers it a send gesture.
4. **Explicit send remains explicit.** When `composer/sendWithCtrlEnter` is enabled, Ctrl+Enter sends from every editor context, including lists, quotes, code blocks and tables.
5. **One user gesture is one undo step.** Splitting a list, leaving a quote, adding/removing a table row, etc. must be an atomic edit transaction.
6. **Caret and selection are part of the command result.** Every structural command specifies where the caret lands afterwards.
7. **No pixel/layout hacks determine structure.** Decisions use `QTextCursor`, `QTextBlock`, `QTextList`, `QTextTable` and format properties.
8. **Qt-version behavior is tested, not assumed.** MatterLeast supports Qt 5.15 and Qt 6. Markdown serialization of lists, quotes, code blocks and especially tables must have cross-version tests.

## Key-event precedence

Key handling follows this order:

1. completion popup/navigation owns its keys while active;
2. an explicit configured send chord (`Ctrl+Enter` when Ctrl+Enter-to-send is enabled) sends the message;
3. the active structural element gets first refusal for Enter, Shift+Enter, Tab, Backtab, Backspace, Delete and boundary arrows;
4. link/inline-format escape behavior runs;
5. ordinary editor behavior runs;
6. only an otherwise-unhandled Enter may become the default send-on-Enter gesture.

Consequences:

- In the default send-on-Enter mode, a user editing a list/table/quote/code block does not accidentally send while trying to extend the structure.
- In that mode, keyboard sending from inside a structure is intentionally unavailable; leave the structure or click Send. Existing Ctrl+Enter=newline semantics are preserved.
- In Ctrl+Enter-to-send mode, Ctrl+Enter is an unambiguous global send command and Enter remains available for structural editing.

## Command context

Before executing any toolbar or keyboard command, derive a context containing at least:

- current cursor and selection;
- selected block range;
- current `QTextList`, if any;
- list item depth/style;
- current block quote level;
- current code-fence state/language;
- current `QTextTable` and table cell/row/column;
- active inline formats and whether a selection is mixed;
- whether the command is operating at a structural boundary (start/end/empty item/cell/block).

This context is the input to semantic editor commands. UI code should not duplicate those tests.

## Inline formatting

Applies to bold, italic, strike-through and inline code.

### Selection

- If a non-empty selection is entirely formatted with the requested style, the command removes that style.
- Otherwise it applies the style to the whole selection.
- Mixed selections therefore converge to a predictable state on the first click and toggle off on the second.
- Structural block metadata must not be changed by an inline command.

### Collapsed cursor

- Toggling a style changes the format used by subsequently typed text without inserting placeholder characters.
- The pending/current format remains active until explicitly toggled off or the editor enters a context that must reset it.
- A paragraph/list-item split carries ordinary emphasis in the normal QTextEdit way, but link formatting has explicit exit rules below.

## Links

- With selected plain text, Insert Link preserves the text as the link label.
- With no selection, the URL dialog may use the URL as the initial label.
- When the cursor/selection is already in a link, the command edits that link instead of nesting another link.
- Remove Link preserves the label.
- Double-space immediately after linked text converts the inherited linked space into one ordinary space and consumes the second space. This is the established explicit "continue outside link" gesture.
- Pasting one HTTP(S) URL over selected plain text creates a link instead of replacing the label.
- A link must never leak into a newly created structural block merely because the cursor was at the end of linked text.

## Paragraphs and sending

### Default send-on-Enter mode

- Enter in an ordinary paragraph sends.
- Shift+Enter inserts a line break/new paragraph according to the editor's existing newline behavior.
- Ctrl+Enter inserts a newline, preserving the existing MatterLeast setting semantics.
- Enter in a structural element is handled by that element and does **not** send.

### Ctrl+Enter-to-send mode

- Enter inserts/continues document structure.
- Shift+Enter inserts a hard/explicit line break where applicable.
- Ctrl+Enter sends from every context.

## Lists

The same rules apply to unordered and ordered lists. Switching the toolbar action from bullet to numbered (or vice versa) changes list style in place rather than destroying item structure.

### Toolbar command

- On ordinary paragraph(s): convert the selected/current blocks to the requested list type.
- On a list of the same type: toggle the selected/current top-level item(s) back to ordinary paragraph(s).
- On a list of the other type: change the list style while preserving item text and nesting.
- With a multi-block selection, apply the command to the complete selected blocks, not partial visual lines.

### Enter

- Non-empty item: split at the cursor. The right side becomes a new item of the same type and indentation.
- Empty **top-level** item: exit the list at that exact position. If the item is in the middle, preserve the list before and after it and place an empty ordinary paragraph between them.
- Empty **nested** item: outdent one level instead of leaving the list completely.
- Shift+Enter: insert a hard line break inside the current item; never split/outdent/exit the list.

### Tab / Shift+Tab

- Tab indents the current item one level when structurally valid.
- Shift+Tab/Backtab outdents one level.
- With a selection spanning several list items, indent/outdent the selected range as one command.
- Indenting an item moves its nested descendants with it; an item's subtree must never be orphaned.
- Invalid indentation (for example no preceding item to become a parent) is a no-op.

### Backspace

At the start of an item with no selection:

- empty nested item: outdent one level;
- empty top-level item in a multi-item list: remove the item and move the caret to the adjacent item;
- only empty item in the list: convert the list block to an ordinary empty paragraph;
- non-empty nested item: outdent that item/subtree one level;
- non-empty top-level item: unlist only that item, preserving list blocks on either side.

### Delete

At the end of an item with no selection:

- merge with the next list item when one exists;
- otherwise merge with the following compatible text block.

### Arrow navigation

Normal QTextEdit visual movement is retained. At a structural boundary, Up/Down may cross to the previous/next item/block while preserving the preferred horizontal caret position.

## Block quotes

Quotes follow the established AnyKeep "Enter twice to leave" behavior.

### Toolbar command

- Ordinary selected/current blocks become quoted.
- Quoted selected/current blocks toggle back to ordinary paragraphs.
- Partial selections expand to complete affected blocks.

### Enter

- Enter in non-empty quote content continues the quote and creates a trailing quoted line/paragraph.
- Enter again on that empty trailing quoted line removes quote formatting from the empty line and leaves the caret in an ordinary paragraph after the quote.
- Shift+Enter always inserts a line break inside the quote and never terminates it.

### Backspace

- At the start of a non-empty quoted block, Backspace removes one quote level rather than merging unpredictably with preceding content.
- An empty quote collapses to an ordinary empty paragraph; when it is an isolated structural block, focus/caret moves backwards consistently with other empty-block removal commands.

## Code blocks

A fenced code block is a structural context, not inline code.

### Toolbar command

- Ordinary selected/current blocks are converted to one fenced code block.
- Invoking the command inside a fenced code block converts it back to ordinary paragraph content.
- The command preserves selected text verbatim; rich inline formatting is not carried into code.

### Keyboard behavior

- Enter inserts a newline inside the code block and never triggers default send.
- Shift+Enter also inserts a newline; there is no structural split distinction inside code.
- Tab indents the current line or every selected line by four spaces.
- Shift+Tab/Backtab removes one tab or up to four leading spaces from each affected line.
- Backspace on an entirely empty code block removes the code-block structure and leaves an editable ordinary paragraph / moves backwards according to surrounding content.
- Delete on an empty code block remains an ordinary editing key; empty-code structural removal is Backspace-only, matching AnyKeep.

Language selection and visual line wrapping are presentation metadata/actions and must not alter the canonical code text. If language selection is added, it maps to the Markdown fence language.

## Tables

Tables must be implemented only after Qt 5.15 and Qt 6 round-trip tests demonstrate that our chosen `QTextTable` operations serialize to Mattermost-compatible GitHub-flavoured Markdown.

### Creation

- The initial table is **2 × 2**, matching AnyKeep's established default.
- The table command inserts a real `QTextTable`, never pipe characters pretending to be a table inside ordinary text.
- After insertion, focus moves to the first cell.
- Insertion is one undoable transaction.

### Context menu / structural actions

When the cursor is in a table cell, expose:

- Insert row above
- Insert row below
- Delete row (enabled only when more than one row exists)
- Insert column left
- Insert column right
- Delete column (enabled only when more than one column exists)
- Delete table

Row/column operations preserve the logical current cell when possible and are one undo step each.

### Tab navigation

- Tab moves to the next cell.
- Shift+Tab moves to the previous cell.
- Tab in the final cell appends a new row and focuses its first cell.
- Shift+Tab in the first cell does not create structure before the table.

### Left / Right

With no selection:

- Left at the beginning of a cell moves to the end of the previous cell.
- Right at the end of a cell moves to the start of the next cell.

Inside cell text, ordinary character navigation wins.

### Up / Down

- At a visual top/bottom boundary, Up/Down moves to the same column in the previous/next row while preserving preferred horizontal position.
- Up above the first row moves to the preceding document block.
- Down below the last row moves to the following document block.
- Auto-repeat navigation must not keep creating new document blocks.

### Enter / Shift+Enter

- Shift+Enter inserts an explicit line break inside the current cell.
- Enter at the **end** of a cell inserts a new row below the current row and focuses the same column in that new row.
- Enter elsewhere in cell content remains an intra-cell edit if Markdown serialization supports it safely; cross-version tests decide the exact representation.
- If the final row is entirely empty, has more than one row, and Enter is pressed in an empty cell of that row, remove the empty row and leave the table into the following ordinary paragraph. This is the table equivalent of Enter on an empty top-level list item.

### Backspace / Delete

- If the entire table is empty, Backspace or Delete from an empty cell removes the table. Backspace places the caret before; Delete places it after.
- If the current row is entirely empty and the table has more than one row, Backspace/Delete in an empty cell removes that row.
- Backspace in an otherwise empty cell moves to the previous cell (or the preceding block from the first cell) rather than merging cell structure.
- Row/column keyboard deletion must never reduce dimensions below 1 × 1; deleting the structural table itself is a separate command.

## Table serialization gate

Before enabling the table toolbar action, unit tests must cover at least:

- parse Markdown table -> `QTextTable` -> Markdown on Qt 5.15 and Qt 6;
- 2 × 2 table creation and serialization;
- adding/removing rows and columns;
- escaping pipes/backslashes in cells;
- inline formatting and links in cells;
- Shift+Enter / multiline-cell representation;
- empty cells;
- leaving a table and preserving the following paragraph.

If Qt's serializer differs materially between supported versions, MatterLeast must own a small table Markdown serializer instead of accepting version-dependent message text.

## Structural selection and toolbar state

Toolbar state is derived from command context, not from the last clicked button:

- inline buttons are checked only when the whole selection has the format;
- list button reflects current list style;
- quote/code buttons reflect current block structure;
- mixed structural selections show a neutral/mixed state rather than lying about the selection;
- table actions are available only while the cursor/selection is inside one table.

## Undo/redo

Every semantic command must be grouped into one QTextDocument edit block (`beginEditBlock()` / `endEditBlock()` or equivalent):

- list split/exit/outdent;
- quote exit;
- code conversion;
- table create/delete;
- row/column insert/delete;
- multi-item indent/outdent;
- toolbar conversion over a block selection.

Undo restores both content and structure. After undo/redo the cursor should be restored by QTextEdit where possible; commands must not perform a second unrelated edit merely to repair formatting.

## Implementation boundary

The target architecture is:

- `MessageTextEditWidget` owns the document and exposes canonical Markdown;
- a dedicated rich-text interaction/command layer derives structural context and executes semantic commands;
- toolbar, keyboard and context menu call that command layer;
- message-send policy runs only after the command layer declines the key;
- serialization tests form the boundary between Qt document structures and Mattermost Markdown.

Do not add a new rich-text feature by directly manipulating formats from the toolbar without also defining its keyboard, boundary, exit, deletion, navigation, selection, undo and Markdown-roundtrip behavior here.
