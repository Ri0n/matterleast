#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>

class QStackedWidget;

namespace Mattermost {

class Backend;
class BackendChannel;
class ChatArea;

// The central channel surface owns its pages independently of any sidebar
// projection. Live channel objects are unique within their owning Storage;
// semantic account/channel IDs remain the responsibility of navigation.
class ChannelPageHost final : public QObject
{
    Q_OBJECT
public:
    static ChannelPageHost& instance(QStackedWidget& stack);

    ChatArea* currentPage() const;
    ChatArea* pageFor(const BackendChannel& channel) const;
    ChatArea* activate(Backend& backend, BackendChannel& channel,
                       const QString& teamId);

signals:
    void channelActivated();

private:
    explicit ChannelPageHost(QStackedWidget& stack);
    void removePage(const BackendChannel* channel);

    QStackedWidget& _stack;
    QHash<const BackendChannel*, QPointer<ChatArea>> _pages;
};

} // namespace Mattermost
