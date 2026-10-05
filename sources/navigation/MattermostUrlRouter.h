#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

namespace Mattermost {

struct MattermostUrlRoute
{
    enum class Kind {
        External,
        LocalOther,
        Channel,
        DirectMessage,
        GroupMessage,
        Post
    };

    Kind kind = Kind::External;
    QString teamName;
    QString target;
    QUrl resolvedUrl;
};

class MattermostUrlRouter final
{
public:
    static MattermostUrlRoute classify(const QUrl& url, const QUrl& serverUrl)
    {
        MattermostUrlRoute result;
        if (!url.isValid() || !serverUrl.isValid() || serverUrl.host().isEmpty()
            || !isHttpScheme(serverUrl.scheme())) {
            return result;
        }

        const QUrl resolvedUrl = url.isRelative() ? serverUrl.resolved(url) : url;
        if (!resolvedUrl.isValid() || resolvedUrl.host().isEmpty()
            || !resolvedUrl.userInfo().isEmpty()
            || !isHttpScheme(resolvedUrl.scheme())
            || !sameOrigin(resolvedUrl, serverUrl)) {
            return result;
        }

        QString basePath = serverUrl.path();
        while (basePath.size() > 1 && basePath.endsWith(QLatin1Char('/'))) {
            basePath.chop(1);
        }
        if (basePath == QStringLiteral("/")) {
            basePath.clear();
        }

        QString routePath = resolvedUrl.path();
        if (!basePath.isEmpty()) {
            if (routePath == basePath) {
                routePath.clear();
            } else if (routePath.startsWith(basePath + QLatin1Char('/'))) {
                routePath.remove(0, basePath.size());
            } else {
                return result;
            }
        }

        result.resolvedUrl = resolvedUrl;
        result.kind = MattermostUrlRoute::Kind::LocalOther;

        const QStringList path =
            routePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (path.size() != 3) {
            return result;
        }

        result.teamName = path.at(0);
        result.target = path.at(2);
        if (result.teamName.isEmpty() || result.target.isEmpty()) {
            result.kind = MattermostUrlRoute::Kind::LocalOther;
            return result;
        }

        if (path.at(1) == QStringLiteral("channels")) {
            result.kind = MattermostUrlRoute::Kind::Channel;
        } else if (path.at(1) == QStringLiteral("messages")) {
            result.kind = result.target.startsWith(QLatin1Char('@'))
                ? MattermostUrlRoute::Kind::DirectMessage
                : MattermostUrlRoute::Kind::GroupMessage;
        } else if (path.at(1) == QStringLiteral("pl")) {
            result.kind = MattermostUrlRoute::Kind::Post;
        }
        return result;
    }

    static QString conversationPath(MattermostUrlRoute::Kind kind,
                                    const QString& teamName,
                                    QString target)
    {
        if (teamName.isEmpty() || target.isEmpty()) {
            return QString();
        }

        if (kind == MattermostUrlRoute::Kind::Channel) {
            return QLatin1Char('/') + teamName + QStringLiteral("/channels/") + target;
        }
        if (kind == MattermostUrlRoute::Kind::DirectMessage) {
            if (!target.startsWith(QLatin1Char('@'))) {
                target.prepend(QLatin1Char('@'));
            }
            return QLatin1Char('/') + teamName + QStringLiteral("/messages/") + target;
        }
        if (kind == MattermostUrlRoute::Kind::GroupMessage) {
            return QLatin1Char('/') + teamName + QStringLiteral("/messages/") + target;
        }
        return QString();
    }

private:
    static bool isHttpScheme(const QString& scheme)
    {
        return scheme.compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
            || scheme.compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0;
    }

    static int effectivePort(const QUrl& url)
    {
        const int explicitPort = url.port(-1);
        if (explicitPort >= 0) {
            return explicitPort;
        }
        if (url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0) {
            return 443;
        }
        if (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0) {
            return 80;
        }
        return -1;
    }

    static bool sameOrigin(const QUrl& left, const QUrl& right)
    {
        return left.scheme().compare(right.scheme(), Qt::CaseInsensitive) == 0
            && left.host().compare(right.host(), Qt::CaseInsensitive) == 0
            && effectivePort(left) == effectivePort(right);
    }
};

} // namespace Mattermost
