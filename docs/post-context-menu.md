# Post context menu and message selection drag

## Single delivery path

`PostContextMenuRouter` (`sources/chat-area/post/PostContextMenuRouter.cpp`) is an application-wide event filter installed via `Q_COREAPP_STARTUP_FUNCTION`. It intercepts **every** `QEvent::ContextMenu` whose receiver is a descendant of a `PostWidget`, forwards it to that `PostWidget` with the original global position, and consumes the original event.

Consequences:

- Child widgets inside a post never see their own `ContextMenu` event. `contextMenuPolicy`, `customContextMenuRequested`, `contextMenuEvent()` overrides and object-level event filters on post children (attachments, `QTextBrowser` viewports, code blocks) are dead code for right-clicks.
- Application event filters run before object event filters, so installing a filter on a child does not get ahead of the router.
- All per-target items must be resolved inside `PostWidget::showPostContextMenu(globalPos)` from the click point.

## Target resolution

`showPostContextMenu` uses `childAt(mapFromGlobal(globalPos))`, the same widget Qt chose as the event receiver:

- Image attachments: the hit widget is `QLabel#imagePreview` inside `AttachedImageFile` (inside the `PostAttachmentList` `QListWidget` viewport). Walk up to `AttachedImageFile`.
- Markdown images such as `![](/api/v4/files/<id>)`: these are rendered inline inside a `MessageContentWidget` `QTextBrowser`, and `PostAttachmentList` skips the matching attachment. `MessageContentWidget::inlineImageAt()` hit-tests with `QAbstractTextDocumentLayout::imageAt()`. Do not hand-roll `cursorForPosition()`/`QTextFragment` geometry: cursor positions are insertion boundaries and resolve to neighbouring text.
- Links are different: `PostWidget::hoveredLink` is hover state set from `QTextBrowser::highlighted`, so it does not depend on context-menu event routing.

`Copy image` puts a decoded `QImage` of the original file (`/api/v4/files/<id>`) on the clipboard and falls back to the rendered preview when the original cannot be downloaded or decoded.

## Message selection drag

The same application filter owns starting a drag-selection of whole messages:

- Any left press inside a post remembers that post as the origin, whichever child received it and whether or not the child accepted it. Ignored presses propagate past the post to the list viewport; that step must not reset the origin.
- While the cursor stays inside the origin post, the press keeps its ordinary meaning (text selection, link activation, image click).
- When the cursor leaves the origin post, the router clears its text selection, starts `ChatLogWidget::beginMessageSelectionDrag()`, and consumes further moves and the release. Later posts join or leave the range as the cursor enters them.
- `PostWidget` has no mouse handlers of its own for this. `PostAttachmentListWidget` ignores mouse input and `AttachedImageFile` opens its preview only on a left click without movement, so attachments do not swallow the drag.
- A press on a link with a scheme still starts a link `QDrag` inside the post. That nested drag loop bypasses message selection by design.

Regression coverage: `tests/PostDragSelectionTest.cpp`, `tests/PostImageContextMenuTest.cpp` right-clicks through the `QWindow`, so Qt performs real hit-testing and the router is active.
