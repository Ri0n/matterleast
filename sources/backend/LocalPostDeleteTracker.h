#pragma once

#include <QSet>
#include <QString>

class QUrl;

namespace Mattermost {

// Tracks locally requested post deletions for exactly one Backend/Storage.
// A WebSocket post_deleted event consumes the matching identity once; no
// process-wide state, timer or global current-account lookup is involved.
class LocalPostDeleteTracker final
{
public:
    void noteRequest(const QUrl& url);
    void clearFailedRequest(const QUrl& url);
    bool take(const QString& postId);
    void clear();

private:
    QSet<QString> pendingPostIds;
};

} // namespace Mattermost
