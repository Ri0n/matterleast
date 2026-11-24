#pragma once

#include <QObject>
#include <QHash>

#include "AccountTrayBadgeModel.h"

class QSystemTrayIcon;

namespace Mattermost {

class Backend;

// Application-owned projection of live account activity onto the tray.
// Neither the selected View nor any sidebar widget participates in counting.
class AccountTrayBadgeMonitor final : public QObject
{
public:
    explicit AccountTrayBadgeMonitor(QSystemTrayIcon& tray);
    void watch(Backend& owner);
    void unwatch(const Backend* owner);
    AggregateTrayBadge aggregate() const { return badges_.aggregate(); }

private:
    void refresh(Backend& owner);
    void updateIcon();

    QSystemTrayIcon& tray_;
    QHash<const Backend*, QObject*> subscriptions_;
    AccountTrayBadgeModel badges_;
    AggregateTrayBadge displayed_;
};

} // namespace Mattermost
