#pragma once

#include <cstdint>

#include <QHash>

class QObject;

namespace Mattermost {

struct AggregateTrayBadge {
    std::uint32_t attentionCount = 0;
    bool hasUnreadChannel = false;
};

// Each authenticated workspace publishes its own badge projection. The tray
// must aggregate all servers: whichever view updates last cannot overwrite
// attention from another (possibly hidden) server.
class AccountTrayBadgeModel final
{
public:
    void update(const QObject* account, std::uint32_t attentionCount,
                bool hasUnreadChannel);
    void remove(const QObject* account);
    AggregateTrayBadge aggregate() const;

private:
    QHash<const QObject*, AggregateTrayBadge> _perAccount;
};

} // namespace Mattermost
