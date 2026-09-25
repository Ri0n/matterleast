#include <QtTest>

#include <QNetworkCookie>
#include <QSslConfiguration>

#include "backend/NetworkRequest.h"

using namespace Mattermost;

class NetworkRequestTest : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        NetworkRequest::clearToken();
        NetworkRequest::setHost(QStringLiteral("https://chat.example.test/"));
    }

    void cleanup()
    {
        NetworkRequest::clearToken();
    }

    void appliesOfficialDesktopProfile()
    {
        NetworkRequest request(QStringLiteral("users/me"));

        QCOMPARE(request.rawHeader("X-Requested-With"),
                 QByteArray("XMLHttpRequest"));
        QCOMPARE(request.rawHeader("Accept"), QByteArray("*/*"));
        QVERIFY(!request.rawHeader("Accept-Language").isEmpty());

        const QByteArray userAgent = request.rawHeader("User-Agent");
        QVERIFY(userAgent.contains("Mattermost/6.3.0"));
        QVERIFY(userAgent.contains("Electron/43.0.0"));
        QVERIFY(userAgent.contains("Chrome/150.0.7871.46"));

        QVERIFY(request.attribute(QNetworkRequest::Http2AllowedAttribute)
                    .toBool());
        QCOMPARE(
            request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
            static_cast<int>(QNetworkRequest::NoLessSafeRedirectPolicy));
        QCOMPARE(request.sslConfiguration().protocol(),
                 QSsl::TlsV1_2OrLater);
    }

    void preservesEndpointSpecificHeaders()
    {
        QNetworkRequest request(
            QUrl(QStringLiteral("https://chat.example.test/api/v4/files")));
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("User-Agent", "custom-agent");

        NetworkRequest::applyDesktopProfile(request);

        QCOMPARE(request.rawHeader("Accept"), QByteArray("application/json"));
        QCOMPARE(request.rawHeader("User-Agent"), QByteArray("custom-agent"));
    }

    void mirrorsSessionCookiesAndCsrf()
    {
        NetworkRequest::setToken(QStringLiteral("session-token"));
        NetworkRequest::updateSessionCookies({
            QNetworkCookie(QByteArrayLiteral("MMUSERID"),
                           QByteArrayLiteral("user-id")),
            QNetworkCookie(QByteArrayLiteral("MMCSRF"),
                           QByteArrayLiteral("csrf-value")),
        });

        QNetworkRequest request(
            QUrl(QStringLiteral("https://chat.example.test/api/v4/posts")));
        NetworkRequest::applyDesktopMutationProfile(request);

        const QByteArray cookie = request.rawHeader("Cookie");
        QVERIFY(cookie.contains("MMAUTHTOKEN=session-token"));
        QVERIFY(cookie.contains("MMUSERID=user-id"));
        QVERIFY(cookie.contains("MMCSRF=csrf-value"));
        QCOMPARE(request.rawHeader("X-CSRF-Token"),
                 QByteArray("csrf-value"));
    }

    void neverLeaksSessionHeadersToAnotherOrigin()
    {
        NetworkRequest::setToken(QStringLiteral("session-token"));
        NetworkRequest::updateSessionCookies({
            QNetworkCookie(QByteArrayLiteral("MMCSRF"),
                           QByteArrayLiteral("csrf-value")),
        });

        QNetworkRequest request(
            QUrl(QStringLiteral("https://example.org/resource")));
        NetworkRequest::applyDesktopMutationProfile(request);

        QVERIFY(request.rawHeader("Cookie").isEmpty());
        QVERIFY(request.rawHeader("X-CSRF-Token").isEmpty());
        QVERIFY(!request.rawHeader("User-Agent").isEmpty());
    }
};

QTEST_APPLESS_MAIN(NetworkRequestTest)
#include "NetworkRequestTest.moc"
