#include <QtTest>

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/AttachmentService.h"
#include "backend/ChannelReferenceService.h"
#include "backend/Storage.h"
#include "chat-area/post/PostPermalinkService.h"
#include "navigation/AppNavigationService.h"
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
                    } else if (requestLine.startsWith("GET /api/v4/config/client?format=old ")) {
                        ++clientConfigRequests;
                        clientConfigCookies.append(cookie);
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("SiteURL"),
                             QStringLiteral("https://canonical-%1.example.test")
                                 .arg(QString::fromUtf8(id))},
                            {QStringLiteral("EnablePermalinkPreviews"), QStringLiteral("true")}
                        }).toJson(QJsonDocument::Compact);
                    } else if (requestLine.startsWith("GET /api/v4/posts/abcdefghijklmnopqrstuvwxyz ")) {
                        ++postLookupRequests;
                        postLookupCookies.append(cookie);
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("id"), QStringLiteral("abcdefghijklmnopqrstuvwxyz")},
                            {QStringLiteral("channel_id"),
                             QStringLiteral("channel-") + QString::fromUtf8(id)},
                            {QStringLiteral("user_id"), QString::fromUtf8(id)},
                            {QStringLiteral("message"), QStringLiteral("origin-") + QString::fromUtf8(id)},
                            {QStringLiteral("create_at"), 1000.0}
                        }).toJson(QJsonDocument::Compact);
                    } else if (requestLine.startsWith("GET /api/v4/channels/channel-")
                               && requestLine.contains("/posts?")) {
                        // After resolving a cold post, PostRepository fetches
                        // a before/after slice. Return a valid empty page, not
                        // a channel record, and track both account-local reads.
                        ++contextPageRequests;
                        contextPageCookies.append(cookie);
                        body = QByteArrayLiteral(
                            R"({"posts":{},"order":[],"prev_post_id":"","next_post_id":""})");
                    } else if (requestLine.startsWith("GET /api/v4/channels/channel-")) {
                        ++channelLookupRequests;
                        channelLookupCookies.append(cookie);
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("id"), QStringLiteral("channel-") + QString::fromUtf8(id)},
                            {QStringLiteral("team_id"), QStringLiteral("team-") + QString::fromUtf8(id)},
                            {QStringLiteral("type"), QStringLiteral("O")},
                            {QStringLiteral("name"), QStringLiteral("shared-channel")},
                            {QStringLiteral("display_name"), QStringLiteral("Distinct Channel")}
                        }).toJson(QJsonDocument::Compact);
                    } else if (requestLine.startsWith("GET /api/v4/teams/team-")) {
                        ++teamLookupRequests;
                        teamLookupCookies.append(cookie);
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("id"), QStringLiteral("team-") + QString::fromUtf8(id)},
                            {QStringLiteral("name"), QStringLiteral("team-") + QString::fromUtf8(id)},
                            {QStringLiteral("display_name"), QStringLiteral("Distinct Team")}
                        }).toJson(QJsonDocument::Compact);
                    } else if (requestLine.startsWith("GET /api/v4/files/shared-file ")) {
                        ++fileRequests;
                        fileCookies.append(cookie);
                        body = QByteArrayLiteral("bytes-") + id;
                    } else if (requestLine.startsWith(
                                   "GET /api/v4/teams/name/shared-team/channels/name/shared-channel ")) {
                        ++referenceRequests;
                        referenceCookies.append(cookie);
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("id"), QString::fromUtf8(id) + QStringLiteral("-channel")},
                            {QStringLiteral("display_name"), QString::fromUtf8(id) + QStringLiteral(" Channel")}
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

                    // The second and subsequent file requests are streaming
                    // downloads. Deliberately attempt to poison Qt's automatic
                    // cookie jar; these cookies must never reach later requests.
                    const QByteArray setCookie =
                        requestLine.startsWith("GET /api/v4/files/shared-file ")
                            && fileRequests >= 2
                            ? QByteArrayLiteral("Set-Cookie: STREAMONLY=")
                                + id + QByteArrayLiteral("\r\n")
                            : QByteArray();
                    socket->write(QByteArrayLiteral("HTTP/1.1 ") + status
                        + QByteArrayLiteral("\r\nContent-Type: application/json\r\n")
                        + setCookie
                        + QByteArrayLiteral("Connection: close"
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
    int clientConfigRequests = 0;
    int postLookupRequests = 0;
    int channelLookupRequests = 0;
    int contextPageRequests = 0;
    QList<QByteArray> contextPageCookies;
    int teamLookupRequests = 0;
    QList<QByteArray> clientConfigCookies;
    QList<QByteArray> postLookupCookies;
    QList<QByteArray> channelLookupCookies;
    QList<QByteArray> teamLookupCookies;
    int fileRequests = 0;
    int referenceRequests = 0;
    QList<QByteArray> fileCookies;
    QList<QByteArray> referenceCookies;
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

        // AttachmentService uses the Backend's HTTP transport (one manager,
        // queue and disk-cache partition per account). The other service
        // retains an error-local connector for expected 404 misses.
        QByteArray firstFile;
        QByteArray secondFile;
        AttachmentService::instance(backendOne).retrieveFile(
            QStringLiteral("shared-file"),
            [&](const QByteArray& data) { firstFile = data; });
        AttachmentService::instance(backendTwo).retrieveFile(
            QStringLiteral("shared-file"),
            [&](const QByteArray& data) { secondFile = data; });
        QTRY_COMPARE(firstFile, QByteArrayLiteral("bytes-user-one"));
        QTRY_COMPARE(secondFile, QByteArrayLiteral("bytes-user-two"));
        QCOMPARE(serverOne.fileRequests, 1);
        QCOMPARE(serverTwo.fileRequests, 1);
        checkCookies(serverOne.fileCookies, "token-one", "token-two");
        checkCookies(serverTwo.fileCookies, "token-two", "token-one");

        QTemporaryDir output;
        QVERIFY(output.isValid());
        for (int iteration = 0; iteration < 2; ++iteration) {
            const QString firstPath = output.filePath(
                QStringLiteral("first-%1.bin").arg(iteration));
            const QString secondPath = output.filePath(
                QStringLiteral("second-%1.bin").arg(iteration));
            bool firstSaved = false;
            bool secondSaved = false;
            QString firstError;
            QString secondError;
            QVERIFY(AttachmentService::instance(backendOne).downloadToFile(
                QStringLiteral("shared-file"), firstPath, {},
                [&](const QString& error) {
                    firstError = error;
                    firstSaved = true;
                }));
            QVERIFY(AttachmentService::instance(backendTwo).downloadToFile(
                QStringLiteral("shared-file"), secondPath, {},
                [&](const QString& error) {
                    secondError = error;
                    secondSaved = true;
                }));
            QTRY_VERIFY(firstSaved && secondSaved);
            QVERIFY2(firstError.isEmpty(), qPrintable(firstError));
            QVERIFY2(secondError.isEmpty(), qPrintable(secondError));
            QFile firstSavedFile(firstPath);
            QFile secondSavedFile(secondPath);
            QVERIFY(firstSavedFile.open(QIODevice::ReadOnly));
            QVERIFY(secondSavedFile.open(QIODevice::ReadOnly));
            QCOMPARE(firstSavedFile.readAll(), QByteArrayLiteral("bytes-user-one"));
            QCOMPARE(secondSavedFile.readAll(), QByteArrayLiteral("bytes-user-two"));
        }
        QCOMPARE(serverOne.fileRequests, 3);
        QCOMPARE(serverTwo.fileRequests, 3);
        checkCookies(serverOne.fileCookies, "token-one", "token-two");
        checkCookies(serverTwo.fileCookies, "token-two", "token-one");
        for (const QByteArray& cookie : serverOne.fileCookies)
            QVERIFY(!cookie.contains("STREAMONLY"));
        for (const QByteArray& cookie : serverTwo.fileCookies)
            QVERIFY(!cookie.contains("STREAMONLY"));

        QString firstChannel;
        QString secondChannel;
        ChannelReferenceService::instance(backendOne).resolve(
            QStringLiteral("shared-team"), QStringLiteral("shared-channel"),
            [&](const QString& name) { firstChannel = name; });
        ChannelReferenceService::instance(backendTwo).resolve(
            QStringLiteral("shared-team"), QStringLiteral("shared-channel"),
            [&](const QString& name) { secondChannel = name; });
        QTRY_COMPARE(firstChannel, QStringLiteral("user-one Channel"));
        QTRY_COMPARE(secondChannel, QStringLiteral("user-two Channel"));
        QCOMPARE(serverOne.referenceRequests, 1);
        QCOMPARE(serverTwo.referenceRequests, 1);
        checkCookies(serverOne.referenceCookies, "token-one", "token-two");
        checkCookies(serverTwo.referenceCookies, "token-two", "token-one");

        // The canonical SiteURL used for native forwarding is independent
        // of the most recently authenticated server's connection origin.
        // Login/startup may independently fetch the client configuration.
        // Count only the additional requests caused by permalink resolution.
        const int firstConfigBaseline = serverOne.clientConfigRequests;
        const int secondConfigBaseline = serverTwo.clientConfigRequests;
        QString firstPermalink;
        QString secondPermalink;
        QString firstLinkError;
        QString secondLinkError;
        PostPermalinkService::instance(backendOne).resolve(
            QStringLiteral("abcdefghijklmnopqrstuvwxyz"), QStringLiteral("shared-team"), true,
            [&](const QString& url, const QString& error) {
                firstPermalink = url;
                firstLinkError = error;
            });
        PostPermalinkService::instance(backendTwo).resolve(
            QStringLiteral("abcdefghijklmnopqrstuvwxyz"), QStringLiteral("shared-team"), true,
            [&](const QString& url, const QString& error) {
                secondPermalink = url;
                secondLinkError = error;
            });
        QTRY_VERIFY(!firstPermalink.isEmpty() && !secondPermalink.isEmpty());
        QVERIFY(firstLinkError.isEmpty());
        QVERIFY(secondLinkError.isEmpty());
        QVERIFY(firstPermalink.startsWith(
            QStringLiteral("https://canonical-user-one.example.test/")));
        QVERIFY(secondPermalink.startsWith(
            QStringLiteral("https://canonical-user-two.example.test/")));
        QCOMPARE(serverOne.clientConfigRequests, firstConfigBaseline + 1);
        QCOMPARE(serverTwo.clientConfigRequests, secondConfigBaseline + 1);
        checkCookies(serverOne.clientConfigCookies, "token-one", "token-two");
        checkCookies(serverTwo.clientConfigCookies, "token-two", "token-one");

        // Neither server has the post's channel or team in Storage. Cold
        // navigation must first fetch the post, channel, and team through
        // the owning Backend; equal post IDs must not cross account boundaries.
        AppNavigationService::instance(backendOne).openPost(QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
        AppNavigationService::instance(backendTwo).openPost(QStringLiteral("abcdefghijklmnopqrstuvwxyz"));
        QTRY_VERIFY(backendOne.getStorage().getChannelById(
            QStringLiteral("channel-user-one")) != nullptr);
        QTRY_VERIFY(backendTwo.getStorage().getChannelById(
            QStringLiteral("channel-user-two")) != nullptr);
        QVERIFY(!backendOne.getStorage().getChannelById(
            QStringLiteral("channel-user-two")));
        QVERIFY(!backendTwo.getStorage().getChannelById(
            QStringLiteral("channel-user-one")));
        // Cold navigation first resolves the missing post; the forced
        // around-post context subsequently validates that same post via
        // PostRepository::loadPost(). Both reads must stay on the owner.
        QTRY_COMPARE(serverOne.postLookupRequests, 2);
        QTRY_COMPARE(serverTwo.postLookupRequests, 2);
        QTRY_COMPARE(serverOne.channelLookupRequests, 1);
        QTRY_COMPARE(serverTwo.channelLookupRequests, 1);
        QTRY_COMPARE(serverOne.contextPageRequests, 2);
        QTRY_COMPARE(serverTwo.contextPageRequests, 2);
        QTRY_COMPARE(serverOne.teamLookupRequests, 1);
        QTRY_COMPARE(serverTwo.teamLookupRequests, 1);
        checkCookies(serverOne.postLookupCookies, "token-one", "token-two");
        checkCookies(serverTwo.postLookupCookies, "token-two", "token-one");
        checkCookies(serverOne.channelLookupCookies, "token-one", "token-two");
        checkCookies(serverTwo.channelLookupCookies, "token-two", "token-one");
        checkCookies(serverOne.contextPageCookies, "token-one", "token-two");
        checkCookies(serverTwo.contextPageCookies, "token-two", "token-one");
        checkCookies(serverOne.teamLookupCookies, "token-one", "token-two");
        checkCookies(serverTwo.teamLookupCookies, "token-two", "token-one");
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
