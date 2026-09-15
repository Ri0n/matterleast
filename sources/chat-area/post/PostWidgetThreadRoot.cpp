#include "PostWidget.h"

#include "chat-area/ChatArea.h"

namespace Mattermost {

bool PostWidget::isThreadRootTimelineAnchor() const
{
    return parentChatArea && parentChatArea->isThread
        && post.id == parentChatArea->root_id;
}

QSize PostWidget::sizeHint() const
{
    return isThreadRootTimelineAnchor()
        ? QSize(0, 1) : QWidget::sizeHint();
}

QSize PostWidget::minimumSizeHint() const
{
    return isThreadRootTimelineAnchor()
        ? QSize(0, 1) : QWidget::minimumSizeHint();
}

int PostWidget::heightForWidth(int width) const
{
    return isThreadRootTimelineAnchor()
        ? 1 : QWidget::heightForWidth(width);
}

} // namespace Mattermost
