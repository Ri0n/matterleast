#pragma once

#include <QNetworkCookieJar>

namespace Mattermost {

// Credential cookies belong exclusively to NetworkSessionContext.
// Never let an independent QNetworkAccessManager silently persist or replay
// Set-Cookie headers into subsequent sessions, including streaming downloads.
// Both Qt 5 and Qt 6 support these virtual methods.
class NetworkSessionCookieJar final : public QNetworkCookieJar
{
public:
    using QNetworkCookieJar::QNetworkCookieJar;

    QList<QNetworkCookie> cookiesForUrl(const QUrl&) const override
    {
        return {};
    }

    bool setCookiesFromUrl(const QList<QNetworkCookie>&, const QUrl&) override
    {
        return false;
    }
};

} // namespace Mattermost
