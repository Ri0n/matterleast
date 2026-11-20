#include <QtTest>

#include <QNetworkCookie>
#include <QSslConfiguration>

#include "backend/NetworkRequest.h"
#include "backend/NetworkSessionContext.h"

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

    void callsPluginRequestUsesServerRootNotApiV4()
    {
        const QString relative = QStringLiteral("plugins/com.mattermost.calls/config");
        NetworkRequest request(QString(), relative);
        QCOMPARE(request.url(),
                 QUrl(QStringLiteral(
                     "https://chat.example.test/plugins/com.mattermost.calls/config")));
        NetworkRequest::setHost(QStringLiteral("https://chat.example.test/mattermost/"));
        NetworkRequest prefixed(QString(), relative);
        QCOMPARE(prefixed.url(), QUrl(QStringLiteral(
                     "https://chat.example.test/mattermost/plugins/com.mattermost.calls/config")));
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

    void isolatedSessionsNeverShareTokensOrCookies()
    {
        NetworkSessionContext first;
        NetworkSessionContext second;
        first.setHost(QStringLiteral("https://one.example.test/mattermost/"));
        second.setHost(QStringLiteral("https://two.example.test/"));
        first.setToken(QStringLiteral("first-secret"));
        second.setToken(QStringLiteral("second-secret"));
        first.updateSessionCookies({
            QNetworkCookie(QByteArrayLiteral("MMCSRF"), QByteArrayLiteral("first-csrf")),
            QNetworkCookie(QByteArrayLiteral("MMUSERID"), QByteArrayLiteral("first-user"))
        });
        second.updateSessionCookies({
            QNetworkCookie(QByteArrayLiteral("MMCSRF"), QByteArrayLiteral("second-csrf"))
        });

        NetworkRequest fromFirst(first, QStringLiteral("posts"));
        NetworkRequest fromSecond(second, QStringLiteral("posts"));
        QCOMPARE(fromFirst.url(), QUrl(QStringLiteral(
            "https://one.example.test/mattermost/api/v4/posts")));
        QCOMPARE(fromSecond.url(), QUrl(QStringLiteral(
            "https://two.example.test/api/v4/posts")));

        first.applyDesktopMutationProfile(fromFirst);
        second.applyDesktopMutationProfile(fromSecond);
        QVERIFY(fromFirst.rawHeader("Cookie").contains("first-secret"));
        QVERIFY(fromFirst.rawHeader("Cookie").contains("first-user"));
        QVERIFY(!fromFirst.rawHeader("Cookie").contains("second-secret"));
        QCOMPARE(fromFirst.rawHeader("X-CSRF-Token"), QByteArray("first-csrf"));
        QVERIFY(fromSecond.rawHeader("Cookie").contains("second-secret"));
        QVERIFY(!fromSecond.rawHeader("Cookie").contains("first-secret"));
        QCOMPARE(fromSecond.rawHeader("X-CSRF-Token"), QByteArray("second-csrf"));

        // A legacy login cannot modify an explicitly bound session.
        NetworkRequest::setHost(QStringLiteral("https://legacy.example.test/"));
        NetworkRequest::setToken(QStringLiteral("legacy-secret"));
        NetworkRequest later(first, QStringLiteral("users/me"));
        QCOMPARE(later.url(), QUrl(QStringLiteral(
            "https://one.example.test/mattermost/api/v4/users/me")));
        QVERIFY(later.rawHeader("Cookie").contains("first-secret"));
        QVERIFY(!later.rawHeader("Cookie").contains("legacy-secret"));
    }

    void perSessionLogoutAndOriginChecksRemainIsolated()
    {
        NetworkSessionContext first;
        NetworkSessionContext second;
        first.setHost(QStringLiteral("https://one.example.test/"));
        second.setHost(QStringLiteral("https://two.example.test/"));
        first.setToken(QStringLiteral("first-secret"));
        second.setToken(QStringLiteral("second-secret"));

        QNetworkRequest foreign(QUrl(QStringLiteral(
            "https://two.example.test/api/v4/posts")));
        first.applyDesktopMutationProfile(foreign);
        QVERIFY(foreign.rawHeader("Cookie").isEmpty());
        QVERIFY(foreign.rawHeader("X-CSRF-Token").isEmpty());

        first.clearToken();
        QVERIFY(NetworkRequest(first, QStringLiteral("users/me"))
                    .rawHeader("Cookie").isEmpty());
        QVERIFY(NetworkRequest(second, QStringLiteral("users/me"))
                    .rawHeader("Cookie").contains("second-secret"));

        first.setHost(QStringLiteral("https://other.example.test/"));
        first.setToken(QStringLiteral("new-secret"));
        QVERIFY(NetworkRequest(first, QStringLiteral("users/me"))
                    .rawHeader("Cookie").contains("new-secret"));
        QVERIFY(NetworkRequest(second, QStringLiteral("users/me"))
                    .rawHeader("Cookie").contains("second-secret"));
    }

    void perSessionCookieReplacementAndRemoval()
    {
        NetworkSessionContext session;
        session.setHost(QStringLiteral("https://example.test/"));
        session.setToken(QStringLiteral("login-token"));
        session.updateSessionCookies({
            QNetworkCookie(QByteArrayLiteral("MMCSRF"), QByteArrayLiteral("old")),
            QNetworkCookie(QByteArrayLiteral("MMUSERID"), QByteArrayLiteral("user"))
        });
        session.updateSessionCookies({
            QNetworkCookie(QByteArrayLiteral("mmcsrf"), QByteArrayLiteral("new"))
        });
        QNetworkRequest request = session.request(
            QStringLiteral("api/v4/"), QStringLiteral("posts"));
        session.applyDesktopMutationProfile(request);
        QCOMPARE(request.rawHeader("X-CSRF-Token"), QByteArray("new"));
        QVERIFY(!request.rawHeader("Cookie").contains("old"));
        session.updateSessionCookies({
            QNetworkCookie(QByteArrayLiteral("MMCSRF"), QByteArray())
        });
        QNetworkRequest afterRemoval = session.request(
            QStringLiteral("api/v4/"), QStringLiteral("posts"));
        session.applyDesktopMutationProfile(afterRemoval);
        QVERIFY(afterRemoval.rawHeader("X-CSRF-Token").isEmpty());
        QVERIFY(afterRemoval.rawHeader("Cookie").contains("MMAUTHTOKEN=login-token"));
    }

    void accountCacheNamespaceIsStableAndScoped()
    {
        NetworkSessionContext first;
        NetworkSessionContext second;
        first.setHost(QStringLiteral("https://one.example.test/"));
        second.setHost(QStringLiteral("https://two.example.test/"));
        const QString firstKey = first.cachePartitionId(QStringLiteral("alice"));
        QVERIFY(!firstKey.isEmpty());
        QCOMPARE(firstKey, first.cachePartitionId(QStringLiteral("Alice")));
        QVERIFY(firstKey != first.cachePartitionId(QStringLiteral("bob")));
        QVERIFY(firstKey != second.cachePartitionId(QStringLiteral("alice")));
        QVERIFY(!firstKey.contains(QStringLiteral("alice")));
        QVERIFY(!firstKey.contains(QStringLiteral("example.test")));
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
