#pragma once

#include <functional>
#include <QHash>
#include <QObject>

class QWidget;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
class NotificationManager;

// Observe account post events independently of any window or active View.
// The frontend supplies only exact-destination visibility and the current
// top-level urgency target; it does not own the notification subscription.
class AccountPostNotifications final : public QObject
{
public:
    using Visibility = std::function<bool(Backend&, const QString&, const QString&)>;
    using Window = std::function<QWidget*()>;

    AccountPostNotifications(NotificationManager& notifications, Visibility visible,
                             Window window, QObject* parent = nullptr);
    ~AccountPostNotifications() override;

    void watch(Backend& source);
    void unwatch(const Backend* source);

private:
    void notify(Backend& source, BackendChannel& channel, const BackendPost& post);

    NotificationManager& notifications_;
    Visibility visible_;
    Window window_;
    QHash<const Backend*, QObject*> subscriptions_;
};

} // namespace Mattermost
