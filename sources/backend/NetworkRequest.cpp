/**
 * @file NetworkRequest.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Jan 23, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "NetworkRequest.h"

#include <QLocale>
#include <QSslConfiguration>
#include <QUrl>

namespace Mattermost {

QString NetworkRequest::httpHost;
QString NetworkRequest::httpToken;
QList<QNetworkCookie> NetworkRequest::httpCookies;

const QString NetworkRequest::mattermostMain = "api/v4/";
const QString NetworkRequest::matterpoll = "plugins/com.github.matterpoll.matterpoll/api/v1/";

namespace {

bool sameHeaderName(const QByteArray& left, const QByteArray& right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

QString preferredLanguage()
{
    const QStringList languages = QLocale::system().uiLanguages();
    if (!languages.isEmpty() && !languages.first().isEmpty()) {
        return languages.first();
    }

    QString fallback = QLocale::system().name();
    fallback.replace(QLatin1Char('_'), QLatin1Char('-'));
    return fallback == QStringLiteral("C") ? QString() : fallback;
}

} // namespace

NetworkRequest::NetworkRequest () = default;

NetworkRequest::NetworkRequest (const QString& url, bool useCache)
    : NetworkRequest(mattermostMain, url, useCache)
{
}

NetworkRequest::NetworkRequest(const QString& urlRoot,
                               const QString& url,
                               bool useCache)
{
    setUrl(httpHost + urlRoot + url);
    applyDesktopProfile(*this);
    setAttribute(QNetworkRequest::CacheSaveControlAttribute, useCache);
}

NetworkRequest::~NetworkRequest () = default;

void NetworkRequest::setHost(const QString& host)
{
    QString nextHost = host;
    if (!nextHost.endsWith(QLatin1Char('/'))) {
        nextHost.append(QLatin1Char('/'));
    }

    if (!httpHost.isEmpty() && httpHost != nextHost) {
        clearToken();
    }
    httpHost = nextHost;
}

void NetworkRequest::clearToken()
{
    httpToken.clear();
    httpCookies.clear();
}

void NetworkRequest::setToken(const QString& token)
{
    httpToken = token;
}

const QString& NetworkRequest::getToken()
{
    return httpToken;
}

const QString& NetworkRequest::host()
{
    return httpHost;
}

void NetworkRequest::updateSessionCookies(
    const QList<QNetworkCookie>& cookies)
{
    for (const QNetworkCookie& cookie : cookies) {
        const QByteArray name = cookie.name();
        if (name.isEmpty()) {
            continue;
        }

        for (auto it = httpCookies.begin(); it != httpCookies.end();) {
            if (sameHeaderName(it->name(), name)) {
                it = httpCookies.erase(it);
            } else {
                ++it;
            }
        }

        // Empty values in Set-Cookie are logout/expiry updates.
        if (!cookie.value().isEmpty()) {
            httpCookies.push_back(cookie);
        }
    }
}

QByteArray NetworkRequest::desktopUserAgent()
{
    // Current supported official Mattermost Desktop reference:
    // v6.3.0 -> Electron 43.0.0 -> Chromium 150.0.7871.46.
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

bool NetworkRequest::isMattermostOrigin(const QUrl& url)
{
    const QUrl base(httpHost);
    if (!base.isValid() || !url.isValid()) {
        return false;
    }

    const int basePort = base.port(
        base.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
            ? 443 : 80);
    const int urlPort = url.port(
        url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
            ? 443 : 80);

    return base.scheme().compare(url.scheme(), Qt::CaseInsensitive) == 0
        && base.host().compare(url.host(), Qt::CaseInsensitive) == 0
        && basePort == urlPort;
}

QByteArray NetworkRequest::cookieHeaderValue()
{
    QByteArrayList values;
    bool hasAuthToken = false;

    for (const QNetworkCookie& cookie : httpCookies) {
        if (cookie.name().isEmpty() || cookie.value().isEmpty()) {
            continue;
        }
        hasAuthToken = hasAuthToken
            || sameHeaderName(cookie.name(), QByteArrayLiteral("MMAUTHTOKEN"));
        values.push_back(cookie.name() + '=' + cookie.value());
    }

    // Token-only logins do not pass through /users/login and therefore may not
    // have a browser cookie jar snapshot. Preserve the existing session-token
    // behavior as the fallback.
    if (!hasAuthToken && !httpToken.isEmpty()) {
        values.prepend(
            QByteArrayLiteral("MMAUTHTOKEN=") + httpToken.toUtf8());
    }

    return values.join(QByteArrayLiteral("; "));
}

QByteArray NetworkRequest::csrfToken()
{
    for (const QNetworkCookie& cookie : httpCookies) {
        if (sameHeaderName(cookie.name(), QByteArrayLiteral("MMCSRF"))) {
            return cookie.value();
        }
    }
    return {};
}

void NetworkRequest::applyDesktopProfile(QNetworkRequest& request)
{
    if (request.rawHeader("User-Agent").isEmpty()) {
        request.setRawHeader("User-Agent", desktopUserAgent());
    }
    if (request.rawHeader("X-Requested-With").isEmpty()) {
        request.setRawHeader("X-Requested-With", "XMLHttpRequest");
    }
    if (request.rawHeader("Accept").isEmpty()) {
        request.setRawHeader("Accept", "*/*");
    }
    if (request.rawHeader("Accept-Language").isEmpty()) {
        const QString language = preferredLanguage();
        if (!language.isEmpty()) {
            request.setRawHeader("Accept-Language", language.toUtf8());
        }
    }

    if (isMattermostOrigin(request.url())
        && request.rawHeader("Cookie").isEmpty()) {
        const QByteArray cookie = cookieHeaderValue();
        if (!cookie.isEmpty()) {
            request.setRawHeader("Cookie", cookie);
        }
    }

    // Qt 5.15 defaults Http2AllowedAttribute to false while Qt 6 defaults it
    // to true. Explicitly opt in on every request so both builds negotiate the
    // same protocol and fall back to HTTP/1.1 only when the peer lacks h2.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy));

    // Match the modern browser floor. TLS 1.3 remains available when the Qt
    // backend and server support it.
    QSslConfiguration ssl = request.sslConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(ssl);
}

void NetworkRequest::applyDesktopMutationProfile(QNetworkRequest& request)
{
    applyDesktopProfile(request);

    if (!isMattermostOrigin(request.url())
        || !request.rawHeader("X-CSRF-Token").isEmpty()) {
        return;
    }

    const QByteArray csrf = csrfToken();
    if (!csrf.isEmpty()) {
        request.setRawHeader("X-CSRF-Token", csrf);
    }
}

} /* namespace Mattermost */
