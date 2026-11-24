#include "ChannelPageHost.h"

#include <QStackedWidget>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"

namespace Mattermost {

ChannelPageHost& ChannelPageHost::instance(QStackedWidget& stack)
{
    if (auto* existing = stack.findChild<ChannelPageHost*>(
            QString(), Qt::FindDirectChildrenOnly))
        return *existing;
    return *new ChannelPageHost(stack);
}

ChannelPageHost::ChannelPageHost(QStackedWidget& stack)
    : QObject(&stack), _stack(stack)
{
}

ChatArea* ChannelPageHost::currentPage() const
{
    return qobject_cast<ChatArea*>(_stack.currentWidget());
}

ChatArea* ChannelPageHost::pageFor(const BackendChannel& channel) const
{
    return _pages.value(&channel).data();
}

ChatArea* ChannelPageHost::activate(Backend& backend, BackendChannel& channel,
                                   const QString& teamId)
{
    // Never accept a channel belonging to another connection.
    if (backend.getStorage().getChannelById(channel.id) != &channel)
        return nullptr;

    ChatArea* previous = currentPage();
    ChatArea* page = pageFor(channel);
    if (!page) {
        const QString context = teamId.isEmpty() ? backend.getCurrentTeamContextId()
                                                 : teamId;
        page = new ChatArea(backend, channel, context, &_stack, false);
        _pages.insert(&channel, page);
        // Storage may free the channel immediately after onLeave. Retire
        // the page synchronously even if every sidebar is absent or dragging.
        QPointer<ChannelPageHost> guard(this);
        connect(&channel, &BackendChannel::onLeave, page,
                [guard, key = &channel] {
            if (guard) guard->removePage(key);
        });
        connect(page, &QObject::destroyed, this, [this, key = &channel] {
            if (!_pages.value(key)) _pages.remove(key);
        });
        _stack.addWidget(page);
    } else if (!teamId.isEmpty()) {
        page->sidebarTeamId = teamId;
    }

    if (previous && previous != page)
        previous->onDeactivate();
    _stack.setCurrentWidget(page);
    page->onActivate();
    emit channelActivated();
    return page;
}

void ChannelPageHost::removePage(const BackendChannel* channel)
{
    if (ChatArea* page = _pages.take(channel).data()) {
        _stack.removeWidget(page);
        delete page;
    }
}

} // namespace Mattermost
