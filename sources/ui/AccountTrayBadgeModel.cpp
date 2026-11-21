#include "AccountTrayBadgeModel.h"

#include <algorithm>
#include <limits>

namespace Mattermost {

void AccountTrayBadgeModel::update(const QObject* account,
                                   std::uint32_t attentionCount,
                                   bool hasUnreadChannel)
{
    if (account)
        _perAccount.insert(account, {attentionCount, hasUnreadChannel});
}

void AccountTrayBadgeModel::remove(const QObject* account)
{
    _perAccount.remove(account);
}

AggregateTrayBadge AccountTrayBadgeModel::aggregate() const
{
    AggregateTrayBadge result;
    for (const auto& badge : _perAccount) {
        const std::uint32_t remaining =
            std::numeric_limits<std::uint32_t>::max() - result.attentionCount;
        result.attentionCount += std::min(remaining, badge.attentionCount);
        result.hasUnreadChannel |= badge.hasUnreadChannel;
    }
    return result;
}

} // namespace Mattermost
