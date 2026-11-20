#include <QtTest>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendLoginData.h"

using namespace Mattermost;

namespace {

// Two independent local Mattermost API endpoints. This is a Backend-level
// integration fixture: no Docker, external network or credentials are needed.
class FakeMattermostServer final : public QTcpServer
{
public:
    explicit FakeMattermostServer(QByteArray userId)
        : id(std::move(userId))
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray incoming = socket->property("received").toByteArray();
                    incoming += socket->readAll();
                    socket->setProperty("received", incoming);
                    if (!incoming.contains("\r\n\r\n")
                        || socket->property("replied").toBool())
                        return;
                    socket->setProperty("replied", true);

                    const QByteArray requestLine = incoming.left(incoming.indexOf("\r\n"));
                    QByteArray cookie;
                    for (const QByteArray& line : incoming.split('\n')) {
                        if (line.startsWith("Cookie:") || line.startsWith("cookie:"))
                            cookie = line.mid(7).trimmed();
                    }

                    QByteArray body;
                    QByteArray status = QByteArrayLiteral("200 OK");
                    if (requestLine.startsWith("GET /api/v4/users/me ")) {
                        ++identityRequests;
                        identityCookies.append(cookie);
                        if (holdIdentity) return;
                        body = QJsonDocument(QJsonObject{
                            {QStringLiteral("id"), QString::fromUtf8(id)},
                            {QStringLiteral("username"), QStringLiteral("fixture")}
                        }).toJson(QJsonDocument::Compact);
                    } else if (requestLine.startsWith("GET /api/v4/users/")
                               && requestLine.contains("/preferences ")) {
                        ++preferencesRequests;
                        preferenceCookies.append(cookie);
                        body = QByteArrayLiteral("[]");
                    } else if (requestLine.startsWith("GET /api/v4/users/")) {
                        ++profileRequests;
                        profileCookies.append(cookie);
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("id"),
                             QString::fromUtf8(id) + QStringLiteral("-profile")},
                            {QStringLiteral("username"),
                             QStringLiteral("independent-profile")}
                        }).toJson(QJsonDocument::Compact);
                    } else {
                        status = QByteArrayLiteral("404 Not Found");
                        body = QByteArrayLiteral("{}");
                    }

                    socket->write(QByteArrayLiteral("HTTP/1.1 ") + status
                        + QByteArrayLiteral("\r\nContent-Type: application/json"
                                            "\r\nConnection: close"
                                            "\r\nContent-Length: ")
                        + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\n\r\n") + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QString origin() const
    {
        return QStringLiteral("http://127.0.0.1:%1/").arg(serverPort());
    }

    QByteArray id;
    bool holdIdentity = false;
    int identityRequests = 0;
    int preferencesRequests = 0;
    int profileRequests = 0;
    QList<QByteArray> identityCookies;
    QList<QByteArray> preferenceCookies;
    QList<QByteArray> profileCookies;
};

BackendLoginData credentials(const FakeMattermostServer& server,
                             const QString& token)
{
    BackendLoginData data;
    data.domain = server.origin();
    data.username = token;
    data.token = token;
    return data;
}

void checkCookies(const QList<QByteArray>& cookies,
                  const QByteArray& ownToken, const QByteArray& otherToken)
{
    QVERIFY(!cookies.isEmpty());
    for (const QByteArray& cookie : cookies) {
        QVERIFY2(cookie.contains("MMAUTHTOKEN=" + ownToken),
                 "The API request lacked its own server session token");
        QVERIFY2(!cookie.contains(otherToken),
                 "An API request leaked another server's authentication");
    }
}
} // namespace

class BackendMultiServerLoginTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("matterleast-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("backend-multi-server-login-test"));
        QStandardPaths::setTestModeEnabled(true);
    }

    void concurrentlyAuthenticatesTwoIndependentBackends()
    {
        FakeMattermostServer serverOne(QByteArrayLiteral("user-one"));
        FakeMattermostServer serverTwo(QByteArrayLiteral("user-two"));
        QVERIFY(serverOne.listen(QHostAddress::LocalHost));
        QVERIFY(serverTwo.listen(QHostAddress::LocalHost));

        Backend backendOne;
        Backend backendTwo;
        const BackendLoginData loginOne =
            credentials(serverOne, QStringLiteral("token-one"));
        const BackendLoginData loginTwo =
            credentials(serverTwo, QStringLiteral("token-two"));

        bool firstComplete = false;
        bool secondComplete = false;
        QString tokenOne;
        QString tokenTwo;
        backendOne.login(loginOne, [&](const QString& token) {
            tokenOne = token;
            firstComplete = true;
        });
        backendTwo.login(loginTwo, [&](const QString& token) {
            tokenTwo = token;
            secondComplete = true;
        });

        QTRY_VERIFY(firstComplete && secondComplete);
        QCOMPARE(tokenOne, loginOne.token);
        QCOMPARE(tokenTwo, loginTwo.token);
        QVERIFY(backendOne.loggedIn());
        QVERIFY(backendTwo.loggedIn());
        QVERIFY(backendOne.getStorage().loginUser);
        QVERIFY(backendTwo.getStorage().loginUser);
        QCOMPARE(backendOne.getStorage().loginUser->id,
                 QStringLiteral("user-one"));
        QCOMPARE(backendTwo.getStorage().loginUser->id,
                 QStringLiteral("user-two"));

        // Each Backend's immediate post-login preferences request must go
        // through the correct host and carry the correct credentials, even
        // though the legacy static NetworkRequest host now points to serverTwo.
        QTRY_COMPARE(serverOne.preferencesRequests, 1);
        QTRY_COMPARE(serverTwo.preferencesRequests, 1);
        QCOMPARE(serverOne.identityRequests, 1);
        QCOMPARE(serverTwo.identityRequests, 1);
        checkCookies(serverOne.identityCookies, "token-one", "token-two");
        checkCookies(serverTwo.identityCookies, "token-two", "token-one");
        checkCookies(serverOne.preferenceCookies, "token-one", "token-two");
        checkCookies(serverTwo.preferenceCookies, "token-two", "token-one");

        backendOne.retrieveUserPreferences();
        backendTwo.retrieveUserPreferences();
        QTRY_COMPARE(serverOne.preferencesRequests, 2);
        QTRY_COMPARE(serverTwo.preferencesRequests, 2);
        checkCookies(serverOne.preferenceCookies, "token-one", "token-two");
        checkCookies(serverTwo.preferenceCookies, "token-two", "token-one");

        // Both accounts now perform ordinary Backend API operations after
        // the process-wide legacy URL has been replaced by the last login.
        // They must retrieve different server-local profiles, not just
        // different authentication headers sent to the same wrong server.
        QString firstProfile;
        QString secondProfile;
        backendOne.retrieveUser(QStringLiteral("shared-profile-id"),
                                [&](const BackendUser& user) {
            firstProfile = user.id;
        });
        backendTwo.retrieveUser(QStringLiteral("shared-profile-id"),
                                [&](const BackendUser& user) {
            secondProfile = user.id;
        });
        QTRY_COMPARE(firstProfile, QStringLiteral("user-one-profile"));
        QTRY_COMPARE(secondProfile, QStringLiteral("user-two-profile"));
        QCOMPARE(serverOne.profileRequests, 1);
        QCOMPARE(serverTwo.profileRequests, 1);
        checkCookies(serverOne.profileCookies, "token-one", "token-two");
        checkCookies(serverTwo.profileCookies, "token-two", "token-one");
    }

    void cancellationOfOneLoginDoesNotCancelTheOther()
    {
        FakeMattermostServer stalled(QByteArrayLiteral("stalled-user"));
        FakeMattermostServer healthy(QByteArrayLiteral("healthy-user"));
        stalled.holdIdentity = true;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QVERIFY(healthy.listen(QHostAddress::LocalHost));
        Backend waitingBackend;
        Backend activeBackend;

        bool staleCallback = false;
        waitingBackend.login(credentials(stalled, QStringLiteral("stalled-token")),
                             [&](const QString&) { staleCallback = true; });

        QString activeToken;
        bool activeComplete = false;
        activeBackend.login(credentials(healthy, QStringLiteral("active-token")),
                            [&](const QString& token) {
                                activeToken = token;
                                activeComplete = true;
                            });
        QTRY_COMPARE(stalled.identityRequests, 1);
        waitingBackend.cancelPendingLogin();
        QTRY_VERIFY(activeComplete);
        QCOMPARE(activeToken, QStringLiteral("active-token"));
        QVERIFY(!staleCallback);
        QVERIFY(!waitingBackend.loggedIn());
        QVERIFY(activeBackend.loggedIn());

        activeBackend.retrieveUserPreferences();
        QTRY_COMPARE(healthy.preferencesRequests, 2);
        checkCookies(healthy.preferenceCookies, "active-token", "stalled-token");
    }
};

QTEST_MAIN(BackendMultiServerLoginTest)
#include "BackendMultiServerLoginTest.moc"
