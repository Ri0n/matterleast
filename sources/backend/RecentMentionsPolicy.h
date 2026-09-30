#pragma once

#include <utility>

#include <QSet>
#include <QString>
#include <QStringList>

#include "types/BackendUser.h"

namespace Mattermost::RecentMentionsPolicy {

/**
 * Build the all-team Mattermost mention search query for the logged-in user.
 *
 * Mattermost's web client excludes broadcast mentions, ORs the remaining
 * personal mention keys, and quotes every key before passing it to post search.
 * Quoting is significant for keys containing dashes or other search syntax.
 */
inline QString searchTerms(const BackendUser& user)
{
    QStringList keys;
    QSet<QString> seen;

    const auto append = [&keys, &seen](QString key) {
        key = key.trimmed();
        if (key.isEmpty()
            || key == QStringLiteral("@channel")
            || key == QStringLiteral("@all")
            || key == QStringLiteral("@here")
            || seen.contains(key)) {
            return;
        }
        seen.insert(key);
        keys.push_back(std::move(key));
    };

    for (const QString& key : user.notify_preps.mention_keys) {
        append(key);
    }
    if (user.notify_preps.first_name && !user.first_name.isEmpty()) {
        append(user.first_name);
    }
    if (!user.username.isEmpty()) {
        append(QStringLiteral("@") + user.username);
    }

    QStringList quoted;
    quoted.reserve(keys.size());
    for (const QString& key : keys) {
        quoted.push_back(QStringLiteral("\"%1\"").arg(key));
    }
    return quoted.join(QLatin1Char(' '));
}

} // namespace Mattermost::RecentMentionsPolicy
