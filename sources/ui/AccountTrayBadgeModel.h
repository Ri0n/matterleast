#pragma once

#include <cstdint>

#include <QHash>

class QObject;

namespace Mattermost {

struct AggregateTrayBadge {
    std::uint32_t attentionCount = 0;
    bool hasUnreadChannel = false;
};

// Aggregate live account activity independently of the current View.
// Whichever account updates last cannot overwrite another server's state.
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
