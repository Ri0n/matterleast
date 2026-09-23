#include "MentionAutocomplete.h"

#include <algorithm>

#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"

namespace Mattermost {
namespace {

bool startsAtBoundary(const QString& value,
                      const QString& foldedPrefix,
                      const QString& separators)
{
    if (foldedPrefix.isEmpty()) {
        return true;
    }

    const QString folded = value.toCaseFolded();
    if (folded.startsWith(foldedPrefix)) {
        return true;
    }

    for (int index = 1; index < folded.size(); ++index) {
        if (separators.contains(folded.at(index - 1))
            && folded.mid(index).startsWith(foldedPrefix)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool mentionUserMatchesPrefix(const BackendUser& user, const QString& prefix)
{
    const QString foldedPrefix = prefix.trimmed().toCaseFolded();
    if (foldedPrefix.isEmpty()) {
        return true;
    }

    if (startsAtBoundary(user.username, foldedPrefix,
                         QStringLiteral("._-+"))
        || startsAtBoundary(user.first_name, foldedPrefix,
                            QStringLiteral(" "))
        || startsAtBoundary(user.last_name, foldedPrefix,
                            QStringLiteral(" "))
        || startsAtBoundary(user.nickname, foldedPrefix,
                            QStringLiteral(" "))) {
        return true;
    }

    const QString fullName = user.first_name + QLatin1Char(' ') + user.last_name;
    return fullName.toCaseFolded().startsWith(foldedPrefix);
}

QVector<const BackendUser*> localMentionUsers(const BackendChannel& channel,
                                              const QString& prefix,
                                              int limit)
{
    QVector<const BackendUser*> result;
    if (limit <= 0) {
        return result;
    }

    result.reserve(limit);
    for (auto it = channel.members.cbegin();
         it != channel.members.cend() && result.size() < limit;
         ++it) {
        const BackendUser* user = it.value().user;
        if (!user || user->username.isEmpty()
            || !mentionUserMatchesPrefix(*user, prefix)) {
            continue;
        }
        result.push_back(user);
    }
    return result;
}

} // namespace Mattermost
