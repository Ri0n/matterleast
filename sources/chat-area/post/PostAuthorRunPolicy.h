#pragma once

#include <QDateTime>
#include <QString>

#include "backend/types/BackendPost.h"

namespace Mattermost {

/**
 * Pure presentation policy for deciding whether a post continues the author
 * run immediately preceding it in the currently presented timeline.
 */
class PostAuthorRunPolicy
{
public:
    static bool continues(const BackendPost& previous,
                          const BackendPost& current)
    {
        if (previous.isDeleted || current.isDeleted
            || isStructural(previous) || isStructural(current)) {
            return false;
        }

        if (previous.channel_id != current.channel_id
            || previous.root_id != current.root_id) {
            return false;
        }

        if (!sameAuthor(previous, current)) {
            return false;
        }

        // Match the official Mattermost web client: posts collapse only within
        // a five-minute window. Exactly five minutes still belongs to the run.
        if (current.create_at < previous.create_at
            || current.create_at - previous.create_at > CollapseTimeoutMs) {
            return false;
        }

        const QDate previousDay =
            QDateTime::fromMSecsSinceEpoch(previous.create_at).date();
        const QDate currentDay =
            QDateTime::fromMSecsSinceEpoch(current.create_at).date();
        return previousDay.isValid()
            && currentDay.isValid()
            && previousDay == currentDay;
    }

private:
    static constexpr qint64 CollapseTimeoutMs = 1000LL * 60 * 5;

    static bool isStructural(const BackendPost& post)
    {
        return post.type.startsWith(QStringLiteral("system_"));
    }

    static bool sameAuthor(const BackendPost& lhs,
                           const BackendPost& rhs)
    {
        if (!lhs.user_id.isEmpty() || !rhs.user_id.isEmpty()) {
            if (lhs.user_id.isEmpty() || rhs.user_id.isEmpty()
                || lhs.user_id != rhs.user_id) {
                return false;
            }
        } else {
            if (lhs.sender_name.isEmpty()
                || lhs.sender_name != rhs.sender_name) {
                return false;
            }
        }

        return true;
    }
};

} // namespace Mattermost
