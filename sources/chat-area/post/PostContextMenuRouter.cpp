#include <QAbstractButton>
#include <QAbstractSlider>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QPlainTextEdit>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QWidget>

#include "PostWidget.h"
#include "chat-area/ChatLogWidget.h"

namespace Mattermost {
namespace {

PostWidget* enclosingPost(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* post = qobject_cast<PostWidget*>(current)) {
            return post;
        }
        // Qt keeps the parentWidget() chain across top-level child dialogs.
        // A press inside a profile window is not a press on its parent post.
        // Check the post itself first: tests may create a top-level PostWidget.
        if (current->isWindow()) {
            break;
        }
    }
    return nullptr;
}

bool isPostDragControl(QWidget* widget, const PostWidget* post)
{
    // Scrollbars inside code blocks belong to the post's widget tree, but
    // dragging them must never initiate cross-post selection. Likewise, a
    // button or another slider owns its own mouse interaction.
    for (QWidget* current = widget; current && current != post;
         current = current->parentWidget()) {
        if (current->isWindow()
            || qobject_cast<QAbstractSlider*>(current)
            || qobject_cast<QAbstractButton*>(current)) {
            return true;
        }
    }
    return false;
}

ChatLogWidget* enclosingLog(QWidget* widget)
{
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (auto* log = qobject_cast<ChatLogWidget*>(current)) {
            return log;
        }
        if (current->isWindow()) {
            break;
        }
    }
    return nullptr;
}

QPoint globalMousePosition(const QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

class PostContextMenuRouter final : public QObject
{
public:
    explicit PostContextMenuRouter(QObject* parent) : QObject(parent)
    {
        clickTimer_.setSingleShot(true);
        connect(&clickTimer_, &QTimer::timeout, this, [this] {
            QPointer<PostWidget> target = pendingClick_;
            pendingClick_.clear();
            if (target) {
                target->activateDefault();
            }
        });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event) {
            return QObject::eventFilter(watched, event);
        }

        QWidget* widget = qobject_cast<QWidget*>(watched);
        if (!widget) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::ContextMenu) {
            PostWidget* postWidget = enclosingPost(widget);
            if (!postWidget || widget == postWidget) {
                return QObject::eventFilter(watched, event);
            }
            auto* contextEvent = static_cast<QContextMenuEvent*>(event);
            const QPoint postPosition = postWidget->mapFromGlobal(contextEvent->globalPos());
            QContextMenuEvent forwarded(contextEvent->reason(), postPosition,
                                        contextEvent->globalPos(), contextEvent->modifiers());
            QCoreApplication::sendEvent(postWidget, &forwarded);
            contextEvent->accept();
            return true;
        }

        if (event->type() == QEvent::MouseButtonDblClick) {
            cancelDefaultClick();
        }

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                rememberDefaultPress(widget, mouse);
                PostWidget* post = enclosingPost(widget);
                ChatLogWidget* log = post ? enclosingLog(post) : nullptr;
                // An ignored press can propagate from a post into its
                // viewport. Keep that valid origin only for the *same* press
                // inside the original post, never for a new press elsewhere.
                if (post && log && !log->isMessageSelectionMode()
                    && !isPostDragControl(widget, post)) {
                    if (dragOrigin_ != post) {
                        clearDrag();
                        dragOrigin_ = post;
                        dragLog_ = log;
                        rememberTextAnchor(widget, globalMousePosition(mouse));
                    }
                } else if (post || !dragOrigin_
                           || !widget->isAncestorOf(dragOrigin_.data())
                           || !dragOrigin_->rect().contains(
                                  dragOrigin_->mapFromGlobal(globalMousePosition(mouse)))) {
                    clearDrag();
                }
            }
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::MouseMove && clickOrigin_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if ((mouse->buttons() & Qt::LeftButton)
                && (globalMousePosition(mouse) - clickStart_).manhattanLength()
                    >= QApplication::startDragDistance()) {
                clickMoved_ = true;
            }
        }

        if (event->type() == QEvent::MouseMove && dragOrigin_ && dragLog_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!(mouse->buttons() & Qt::LeftButton)) {
                finishDrag();
                clearDrag();
                return QObject::eventFilter(watched, event);
            }

            // Inside the first post the press keeps its ordinary meaning (text
            // selection, links). Leaving it switches to message selection no
            // matter which child received the press.
            const QPoint globalPos = globalMousePosition(mouse);
            const bool insideOrigin = dragOrigin_->rect().contains(
                dragOrigin_->mapFromGlobal(globalPos));
            if (!wholePostDrag_ && insideOrigin) {
                return QObject::eventFilter(watched, event);
            }
            if (wholePostDrag_ && insideOrigin && textEditor_) {
                if (!returningToText_) {
                    dragLog_->suspendMessageSelectionDrag();
                    returningToText_ = true;
                }
                restoreTextSelection(globalPos);
                return true;
            }
            if (returningToText_) {
                returningToText_ = false;
                dragOrigin_->clearTextSelection();
            }

            const QPoint viewportPos = dragLog_->viewport()->mapFromGlobal(globalPos);
            const int index = dragLog_->indexAtViewportPosition(viewportPos.y());
            auto* current = index >= 0
                ? qobject_cast<PostWidget*>(dragLog_->itemWidget(index)) : nullptr;
            if (!wholePostDrag_) {
                wholePostDrag_ = true;
                dragOrigin_->clearTextSelection();
                dragLog_->beginMessageSelectionDrag(
                    dragOrigin_->post.id, current ? current->post.id : dragOrigin_->post.id);
            } else if (current) {
                dragLog_->updateMessageSelectionDrag(current->post.id);
            }
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && dragLog_) {
                const bool consumed = wholePostDrag_;
                if (returningToText_ && textEditor_) {
                    restoreTextSelection(globalMousePosition(mouse));
                }
                finishDrag();
                clearDrag();
                if (consumed) {
                    cancelDefaultClick();
                    return true;
                }
            }
            if (mouse->button() == Qt::LeftButton) {
                clickReleaseSeen_ = true;
                // A release that reaches the post itself was not consumed by
                // any of its children. A press handled by a control is not
                // eligible even if that control ignores its release.
                if (widget == clickOrigin_.data() && clickEligible_
                    && !clickMoved_ && mouse->modifiers() == Qt::NoModifier
                    && clickOrigin_->rect().contains(clickOrigin_->mapFromGlobal(
                           globalMousePosition(mouse)))
                    && !clickOrigin_->wholeMessageSelectionMode()) {
                    QPointer<PostWidget> target = clickOrigin_;
                    const bool selectableText = clickOnText_;
                    cancelDefaultClick();
                    if (selectableText) {
                        // Preserve double-click word selection. Only text
                        // clicks wait for the platform double-click interval.
                        pendingClick_ = target;
                        clickTimer_.start(QApplication::doubleClickInterval());
                    } else if (target) {
                        target->activateDefault();
                    }
                    mouse->accept();
                    return true;
                }
            }
        }

        return QObject::eventFilter(watched, event);
    }

private:
    void cancelDefaultClick()
    {
        clickTimer_.stop();
        pendingClick_.clear();
        clickOrigin_.clear();
        clickReceiver_.clear();
        clickEligible_ = false;
        clickMoved_ = false;
        clickReleaseSeen_ = false;
        clickOnText_ = false;
        clickBlocked_ = false;
    }

    void rememberDefaultPress(QWidget* widget, QMouseEvent* mouse)
    {
        const QPoint position = globalMousePosition(mouse);
        // Qt delivers ignored mouse presses up the widget parent chain. Do
        // not mistake that propagation for a second physical press.
        const bool propagated = clickReceiver_ && !clickReleaseSeen_
            && widget->isAncestorOf(clickReceiver_.data()) && position == clickStart_;
        if (!propagated) {
            cancelDefaultClick();
            clickReceiver_ = widget;
            clickOrigin_ = enclosingPost(widget);
            clickStart_ = position;
            if (!clickOrigin_) {
                return;
            }
            clickBlocked_ = mouse->modifiers() != Qt::NoModifier
                || isPostDragControl(widget, clickOrigin_.data());
            if (auto* edit = qobject_cast<QTextEdit*>(widget->parentWidget())) {
                const QString name = edit->objectName();
                if (edit->viewport() == widget
                    && (name == QStringLiteral("messageRichText")
                        || name == QStringLiteral("messageQuoteRichText"))) {
                    clickOnText_ = true;
                    if (auto* browser = qobject_cast<QTextBrowser*>(edit)) {
                        clickBlocked_ = clickBlocked_ || !browser->anchorAt(
                            widget->mapFromGlobal(position)).isEmpty();
                    }
                }
            } else if (auto* edit = qobject_cast<QPlainTextEdit*>(widget->parentWidget())) {
                clickOnText_ = edit->viewport() == widget
                    && edit->objectName() == QStringLiteral("messageCodeBlock");
            }
        }
        if (clickOrigin_ && !clickBlocked_
            && (widget == clickOrigin_.data() || clickOnText_)) {
            clickEligible_ = true;
        }
    }

    void rememberTextAnchor(QWidget* widget, const QPoint& globalPos)
    {
        // Only the original text viewport can resume a text drag. Controls,
        // attachments and post padding keep ordinary whole-post selection.
        if (auto* browser = qobject_cast<QTextBrowser*>(widget->parentWidget())) {
            if (browser->viewport() == widget
                && !browser->anchorAt(widget->mapFromGlobal(globalPos)).isEmpty()) {
                return;
            }
        }
        if (auto* edit = qobject_cast<QTextEdit*>(widget->parentWidget())) {
            if (edit->viewport() == widget) {
                textEditor_ = edit;
                textAnchor_ = edit->cursorForPosition(widget->mapFromGlobal(globalPos));
            }
        } else if (auto* edit = qobject_cast<QPlainTextEdit*>(widget->parentWidget())) {
            if (edit->viewport() == widget) {
                textEditor_ = edit;
                textAnchor_ = edit->cursorForPosition(widget->mapFromGlobal(globalPos));
            }
        }
    }

    template<typename Edit>
    void restoreSelection(Edit* edit, const QPoint& globalPos)
    {
        if (!edit || textAnchor_.isNull() || textAnchor_.document() != edit->document()) {
            return;
        }
        QTextCursor cursor = textAnchor_;
        cursor.setPosition(edit->cursorForPosition(
            edit->viewport()->mapFromGlobal(globalPos)).position(), QTextCursor::KeepAnchor);
        edit->setTextCursor(cursor);
    }

    void restoreTextSelection(const QPoint& globalPos)
    {
        if (auto* edit = qobject_cast<QTextEdit*>(textEditor_.data())) {
            restoreSelection(edit, globalPos);
        } else if (auto* edit = qobject_cast<QPlainTextEdit*>(textEditor_.data())) {
            restoreSelection(edit, globalPos);
        }
    }

    void finishDrag()
    {
        if (wholePostDrag_ && dragLog_) {
            if (returningToText_) {
                dragLog_->cancelMessageSelection();
            } else {
                dragLog_->finishMessageSelectionDrag();
            }
        }
    }

    void clearDrag()
    {
        dragOrigin_.clear();
        dragLog_.clear();
        wholePostDrag_ = false;
        returningToText_ = false;
        textEditor_.clear();
        textAnchor_ = QTextCursor();
    }

    QPointer<PostWidget> clickOrigin_;
    QPointer<QWidget> clickReceiver_;
    QPointer<PostWidget> pendingClick_;
    QPoint clickStart_;
    QTimer clickTimer_;
    bool clickEligible_ = false;
    bool clickMoved_ = false;
    bool clickReleaseSeen_ = false;
    bool clickOnText_ = false;
    bool clickBlocked_ = false;

    QPointer<PostWidget> dragOrigin_;
    QPointer<ChatLogWidget> dragLog_;
    QPointer<QWidget> textEditor_;
    QTextCursor textAnchor_;
    bool wholePostDrag_ = false;
    bool returningToText_ = false;
};

void installPostContextMenuRouter()
{
    if (!qApp) {
        return;
    }
    auto* router = new PostContextMenuRouter(qApp);
    qApp->installEventFilter(router);
}

Q_COREAPP_STARTUP_FUNCTION(installPostContextMenuRouter)

} // namespace
} // namespace Mattermost
