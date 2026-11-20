#pragma once

#include <QList>
#include <QNetworkCookie>
#include <QNetworkRequest>
#include <QString>

namespace Mattermost {

// The HTTP identity of exactly one authenticated server session. No global
// credentials, shared cookie jar, or implicit current-server lookup.
//
// Multiple Backends must use different instances. Callers can construct
// requests explicitly against this context while the legacy NetworkRequest
// static facade remains available for the existing single-server UI.
class NetworkSessionContext final
{
public:
    void setHost(const QString& host);
    const QString& host() const { return host_; }

    void setToken(const QString& token) { token_ = token; }
    const QString& token() const { return token_; }
    void clearToken();
    void updateSessionCookies(const QList<QNetworkCookie>& cookies);
    // Ignore Set-Cookie from a redirect or response on another origin.
    bool updateSessionCookiesFrom(const QUrl& responseUrl,
                                  const QList<QNetworkCookie>& cookies);

    QNetworkRequest request(const QString& urlRoot, const QString& path,
                            bool useCache = false) const;
    void applyDesktopProfile(QNetworkRequest& request) const;
    void applyDesktopMutationProfile(QNetworkRequest& request) const;

private:
    bool isMattermostOrigin(const QUrl& url) const;
    QByteArray cookieHeaderValue() const;
    QByteArray csrfToken() const;

    QString host_;
    QString token_;
    QList<QNetworkCookie> cookies_;
};

} // namespace Mattermost
