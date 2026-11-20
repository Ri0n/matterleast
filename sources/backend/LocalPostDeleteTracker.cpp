#include "LocalPostDeleteTracker.h"

#include <QStringList>
#include <QUrl>

namespace Mattermost {
namespace {

QString postIdFromDeleteUrl(const QUrl& url)
{
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2 || parts.at(parts.size() - 2) != QStringLiteral("posts")) {
        return {};
    }
    return parts.constLast();
}

} // namespace

void LocalPostDeleteTracker::noteRequest(const QUrl& url)
{
    const QString postId = postIdFromDeleteUrl(url);
    if (!postId.isEmpty())
        pendingPostIds.insert(postId);
}

void LocalPostDeleteTracker::clearFailedRequest(const QUrl& url)
{
    const QString postId = postIdFromDeleteUrl(url);
    if (!postId.isEmpty())
        pendingPostIds.remove(postId);
}

bool LocalPostDeleteTracker::take(const QString& postId)
{
    return !postId.isEmpty() && pendingPostIds.remove(postId);
}

void LocalPostDeleteTracker::clear()
{
    pendingPostIds.clear();
}

} // namespace Mattermost
