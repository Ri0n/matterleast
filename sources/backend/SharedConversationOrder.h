#pragma once

#include <algorithm>

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace Mattermost {

// View-local order of the existing account-qualified DM/GM conversations.
// This is an ephemeral projection, not a second persistent channel model.
// Widget and future QML presenters may feed the same lightweight entries.
struct SharedConversationOrder {
    struct Entry {
        QString accountId;
        QString channelId;
        QString title;
        quint64 activityAt = 0;
    };

    static void sort(QVector<Entry>& entries, const QString& sorting)
    {
        const bool alphabetic = sorting == QLatin1String("alpha");
        std::sort(entries.begin(), entries.end(),
                  [alphabetic](const Entry& a, const Entry& b) {
            if (!alphabetic && a.activityAt != b.activityAt)
                return a.activityAt > b.activityAt;
            const int byTitle = QString::compare(a.title, b.title,
                                                 Qt::CaseInsensitive);
            if (byTitle != 0)
                return byTitle < 0;
            // Remote IDs only identify a conversation within its server.
            // Use a deterministic order for equal names or timestamps.
            const int byAccount = QString::compare(a.accountId, b.accountId);
            if (byAccount != 0)
                return byAccount < 0;
            return QString::compare(a.channelId, b.channelId) < 0;
        });
    }
};

} // namespace Mattermost
