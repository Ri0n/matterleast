#pragma once

#include <QString>
#include <QVector>

#include "SharedConversationOrder.h"

namespace Mattermost {
class Backend;

// Widget-free, View-local projection of existing server-owned conversations.
// No network requests, QObject ownership, model reset or duplicate channel
// storage: only short-lived account-qualified ordering keys.
class SharedConversationProjection final
{
public:
    struct AccountSource {
        QString accountId;
        Backend* backend = nullptr;
    };

    static QVector<SharedConversationOrder::Entry> collect(
        const QVector<AccountSource>& sources, const QString& sorting);
};

} // namespace Mattermost
