#include <QAbstractButton>
#include <QAbstractSlider>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
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
    using QObject::QObject;

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

        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                PostWidget* post = enclosingPost(widget);
                ChatLogWidget* log = post ? enclosingLog(post) : nullptr;
                // An ignored press can propagate from a post into its
                // viewport. Keep that valid origin only for the *same* press
                // inside the original post, never for a new press elsewhere.
                if (post && log && !log->isMessageSelectionMode()
                    && !isPostDragControl(widget, post)) {
                    dragOrigin_ = post;
                    dragLog_ = log;
                    wholePostDrag_ = false;
                } else if (post || !dragOrigin_
                           || !widget->isAncestorOf(dragOrigin_.data())
                           || !dragOrigin_->rect().contains(
                                  dragOrigin_->mapFromGlobal(globalMousePosition(mouse)))) {
                    clearDrag();
                }
            }
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::MouseMove && dragOrigin_ && dragLog_) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (!(mouse->buttons() & Qt::LeftButton)) {
                clearDrag();
                return QObject::eventFilter(watched, event);
            }

            // Inside the first post the press keeps its ordinary meaning (text
            // selection, links). Leaving it switches to message selection no
            // matter which child received the press.
            const QPoint globalPos = globalMousePosition(mouse);
            if (!wholePostDrag_
                && dragOrigin_->rect().contains(dragOrigin_->mapFromGlobal(globalPos))) {
                return QObject::eventFilter(watched, event);
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
            if (mouse->button() == Qt::LeftButton && dragOrigin_) {
                const bool consumed = wholePostDrag_;
                if (wholePostDrag_ && dragLog_) {
                    dragLog_->finishMessageSelectionDrag();
                }
                clearDrag();
                if (consumed) {
                    return true;
                }
            }
        }

        return QObject::eventFilter(watched, event);
    }

private:
    void clearDrag()
    {
        dragOrigin_.clear();
        dragLog_.clear();
        wholePostDrag_ = false;
    }

    QPointer<PostWidget> dragOrigin_;
    QPointer<ChatLogWidget> dragLog_;
    bool wholePostDrag_ = false;
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
