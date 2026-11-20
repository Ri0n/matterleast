#include "NetworkSessionContext.h"

#include <QLocale>
#include <QSslConfiguration>
#include <QUrl>

namespace Mattermost {
namespace {

bool sameHeaderName(const QByteArray& left, const QByteArray& right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

QString preferredLanguage()
{
    const QStringList languages = QLocale::system().uiLanguages();
    if (!languages.isEmpty() && !languages.first().isEmpty())
        return languages.first();

    QString fallback = QLocale::system().name();
    fallback.replace(QLatin1Char('_'), QLatin1Char('-'));
    return fallback == QStringLiteral("C") ? QString() : fallback;
}

QByteArray desktopUserAgent()
{
    // The supported official Mattermost Desktop 6.3.0 reference profile.
#if defined(Q_OS_WIN)
    static const QByteArray platform =
        QByteArrayLiteral("(Windows NT 10.0; Win64; x64)");
#elif defined(Q_OS_MACOS)
    static const QByteArray platform =
        QByteArrayLiteral("(Macintosh; Intel Mac OS X 10_15_7)");
#else
    static const QByteArray platform =
        QByteArrayLiteral("(X11; Linux x86_64)");
#endif
    return QByteArrayLiteral("Mozilla/5.0 ") + platform
        + QByteArrayLiteral(
            " AppleWebKit/537.36 (KHTML, like Gecko)"
            " Chrome/150.0.7871.46 Electron/43.0.0 Safari/537.36"
            " Mattermost/6.3.0");
}

} // namespace

void NetworkSessionContext::setHost(const QString& host)
{
    QString next = host;
    if (!next.endsWith(QLatin1Char('/')))
        next.append(QLatin1Char('/'));
    if (!host_.isEmpty() && host_ != next)
        clearToken();
    host_ = next;
}

void NetworkSessionContext::clearToken()
{
    token_.clear();
    cookies_.clear();
}

void NetworkSessionContext::updateSessionCookies(const QList<QNetworkCookie>& cookies)
{
    for (const QNetworkCookie& cookie : cookies) {
        const QByteArray name = cookie.name();
        if (name.isEmpty()) continue;

        for (auto it = cookies_.begin(); it != cookies_.end();) {
            if (sameHeaderName(it->name(), name))
                it = cookies_.erase(it);
            else
                ++it;
        }
        if (!cookie.value().isEmpty())
            cookies_.append(cookie);
    }
}

bool NetworkSessionContext::updateSessionCookiesFrom(
    const QUrl& responseUrl, const QList<QNetworkCookie>& cookies)
{
    if (!isMattermostOrigin(responseUrl))
        return false;
    updateSessionCookies(cookies);
    return true;
}

QNetworkRequest NetworkSessionContext::request(const QString& urlRoot,
                                               const QString& path,
                                               bool useCache) const
{
    QNetworkRequest result(QUrl(host_ + urlRoot + path));
    applyDesktopProfile(result);
    result.setAttribute(QNetworkRequest::CacheSaveControlAttribute, useCache);
    return result;
}

bool NetworkSessionContext::isMattermostOrigin(const QUrl& url) const
{
    const QUrl base(host_);
    if (!base.isValid() || !url.isValid()) return false;

    const int basePort = base.port(base.scheme().compare(
        QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80);
    const int urlPort = url.port(url.scheme().compare(
        QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80);
    return base.scheme().compare(url.scheme(), Qt::CaseInsensitive) == 0
        && base.host().compare(url.host(), Qt::CaseInsensitive) == 0
        && basePort == urlPort;
}

QByteArray NetworkSessionContext::cookieHeaderValue() const
{
    QByteArrayList values;
    bool hasAuthToken = false;
    for (const QNetworkCookie& cookie : cookies_) {
        if (cookie.name().isEmpty() || cookie.value().isEmpty()) continue;
        hasAuthToken = hasAuthToken ||
            sameHeaderName(cookie.name(), QByteArrayLiteral("MMAUTHTOKEN"));
        values.append(cookie.name() + '=' + cookie.value());
    }
    if (!hasAuthToken && !token_.isEmpty())
        values.prepend(QByteArrayLiteral("MMAUTHTOKEN=") + token_.toUtf8());
    return values.join(QByteArrayLiteral("; "));
}

QByteArray NetworkSessionContext::csrfToken() const
{
    for (const QNetworkCookie& cookie : cookies_) {
        if (sameHeaderName(cookie.name(), QByteArrayLiteral("MMCSRF")))
            return cookie.value();
    }
    return {};
}

void NetworkSessionContext::applyDesktopProfile(QNetworkRequest& request) const
{
    if (request.rawHeader("User-Agent").isEmpty())
        request.setRawHeader("User-Agent", desktopUserAgent());
    if (request.rawHeader("X-Requested-With").isEmpty())
        request.setRawHeader("X-Requested-With", "XMLHttpRequest");
    if (request.rawHeader("Accept").isEmpty())
        request.setRawHeader("Accept", "*/*");
    if (request.rawHeader("Accept-Language").isEmpty()) {
        const QString language = preferredLanguage();
        if (!language.isEmpty())
            request.setRawHeader("Accept-Language", language.toUtf8());
    }

    if (isMattermostOrigin(request.url())
        && request.rawHeader("Cookie").isEmpty()) {
        const QByteArray cookie = cookieHeaderValue();
        if (!cookie.isEmpty())
            request.setRawHeader("Cookie", cookie);
    }

    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy));
    QSslConfiguration ssl = request.sslConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(ssl);
}

void NetworkSessionContext::applyDesktopMutationProfile(QNetworkRequest& request) const
{
    applyDesktopProfile(request);
    if (!isMattermostOrigin(request.url())
        || !request.rawHeader("X-CSRF-Token").isEmpty())
        return;

    const QByteArray csrf = csrfToken();
    if (!csrf.isEmpty())
        request.setRawHeader("X-CSRF-Token", csrf);
}

} // namespace Mattermost
