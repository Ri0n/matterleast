#include "AccountIdentity.h"

#include <QUrl>

namespace Mattermost {

QString accountIdentityKey(const QString& serverBaseUrl,
                                            const QString& serverUserId)
{
    if (serverUserId.isEmpty() || serverUserId.contains(QChar(0x1f)))
        return {};

    QUrl url(serverBaseUrl);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QLatin1String("https")
            && scheme != QLatin1String("http"))
        || !url.userInfo().isEmpty()) {
        return {};
    }

    // Preserve the Mattermost installation path: two installations on the
    // same hostname may be different accounts. Canonicalize only URL spelling.
    url.setQuery(QString());
    url.setFragment(QString());
    if ((scheme == QLatin1String("https") && url.port() == 443)
        || (scheme == QLatin1String("http") && url.port() == 80)) {
        url.setPort(-1);
    }
    QString path = url.path();
    if (path.isEmpty() || !path.endsWith(QLatin1Char('/')))
        path.append(QLatin1Char('/'));
    url.setPath(path);

    return url.toString(QUrl::FullyEncoded) + QChar(0x1f) + serverUserId;
}

} // namespace Mattermost
