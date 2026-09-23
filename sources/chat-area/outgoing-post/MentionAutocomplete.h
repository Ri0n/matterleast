#pragma once

#include <QString>
#include <QVector>

namespace Mattermost {

class BackendChannel;
class BackendUser;

/**
 * Return the small synchronous @mention candidate set that is safe to rebuild
 * on every keystroke. This deliberately inspects channel membership only; the
 * server autocomplete endpoint owns discovery outside the channel.
 */
QVector<const BackendUser*> localMentionUsers(const BackendChannel& channel,
                                              const QString& prefix,
                                              int limit);

bool mentionUserMatchesPrefix(const BackendUser& user, const QString& prefix);

} // namespace Mattermost
