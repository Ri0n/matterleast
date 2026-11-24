#pragma once

#include <functional>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QWidget;
class QSoundEffect;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
class NotificationManager;
class PluginManager;
struct IncomingCall;

// Observe account notifications independently of any window or active View.
// The frontend supplies only exact-destination visibility and the current
// top-level urgency target; it does not own the notification subscription.
class AccountNotifications final : public QObject
{
public:
    using Visibility = std::function<bool(Backend&, const QString&, const QString&)>;
    using Window = std::function<QWidget*()>;

    AccountNotifications(NotificationManager& notifications, PluginManager& plugins,
                             Visibility visible,
                             Window window, QObject* parent = nullptr);
    ~AccountNotifications() override;

    void watch(Backend& source);
    void unwatch(const Backend* source);

private:
    void notify(Backend& source, BackendChannel& channel, const BackendPost& post);
    Backend* sourceForAccount(const QString& accountId) const;
    void incomingCall(const IncomingCall& call);
    void stopRinging(const Backend* source, const QString& callId, quint64 generation = 0);
    void playRingtone();
    void stopRingtoneIfIdle();

    NotificationManager& notifications_;
    PluginManager& plugins_;
    Visibility visible_;
    Window window_;
    struct Subscription {
        QPointer<Backend> source;
        QObject* context = nullptr;
    };
    QHash<const Backend*, Subscription> subscriptions_;
    QHash<const Backend*, QHash<QString, quint64>> ringing_;
    QTimer ringTimer_;
    QSoundEffect* ringtone_ = nullptr;
    quint64 callGeneration_ = 0;
};

} // namespace Mattermost
