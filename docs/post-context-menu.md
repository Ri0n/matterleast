# Post context menu

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

Regression coverage: `tests/PostImageContextMenuTest.cpp` right-clicks through the `QWindow`, so Qt performs real hit-testing and the router is active.
