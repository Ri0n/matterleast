#include <QtTest>

#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/AccountSessionManager.h"
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
                        if (rejectIdentity) {
                            status = QByteArrayLiteral("401 Unauthorized");
                            body = QByteArrayLiteral(R"({"message":"invalid token"})");
                        } else {
                            QString userId = QString::fromUtf8(id);
                            if (separateIdentitiesByToken) {
                                userId += cookie.contains("MMAUTHTOKEN=token-two")
                                    ? QStringLiteral("-two") : QStringLiteral("-one");
                            }
                            body = QJsonDocument(QJsonObject{
                                {QStringLiteral("id"), userId},
                                {QStringLiteral("username"), QStringLiteral("fixture")}
                            }).toJson(QJsonDocument::Compact);
                        }
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
                    } else if (requestLine.startsWith("GET /api/v4/files/shared-file ")
                               || requestLine.startsWith("GET /api/v4/files/legacy-file ")) {
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
                    } else if (requestLine.startsWith("GET /api/v4/users?per_page=200&active=true&page=")) {
                        const bool secondPage = requestLine.contains("page=1 ");
                        ++userPageRequests;
                        const QString pageId = QString::fromUtf8(id)
                            + (secondPage ? QStringLiteral("-page-one")
                                          : QStringLiteral("-page-zero"));
                        body = QJsonDocument(QJsonArray {QJsonObject {
                            {QStringLiteral("id"), pageId},
                            {QStringLiteral("username"), pageId},
                        }}).toJson(QJsonDocument::Compact);
                        if (!secondPage)
                            ++pageZeroResponses;
                    } else if (requestLine.startsWith("GET /api/v4/users/")
                               && requestLine.contains("/preferences ")) {
                        ++preferencesRequests;
                        preferenceCookies.append(cookie);
                        if (rejectPreferences) {
                            status = QByteArrayLiteral("401 Unauthorized");
                            body = QByteArrayLiteral(R"({"message":"unauthorized"})");
                        } else {
                            body = QByteArrayLiteral("[]");
                        }
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
                    const QByteArray cacheControl =
                        requestLine.startsWith("GET /api/v4/files/legacy-file ")
                            ? QByteArrayLiteral("Cache-Control: private, max-age=3600\r\n")
                            : QByteArray();
                    const QByteArray response = QByteArrayLiteral("HTTP/1.1 ")
                        + status
                        + QByteArrayLiteral("\r\nContent-Type: application/json\r\n")
                        + setCookie + cacheControl
                        + QByteArrayLiteral("Connection: close"
                                            "\r\nContent-Length: ")
                        + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\n\r\n") + body;
                    if (holdSecondUserPage
                        && requestLine.startsWith("GET /api/v4/users?per_page=200&active=true&page=1 ")) {
                        heldSecondPages.append(qMakePair(QPointer<QTcpSocket>(socket), response));
                        return;
                    }
                    socket->write(response);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QString origin() const
    {
        return QStringLiteral("http://127.0.0.1:%1/").arg(serverPort());
    }

    void releaseSecondUserPages()
    {
        holdSecondUserPage = false;
        const auto held = std::move(heldSecondPages);
        heldSecondPages.clear();
        for (const auto& item : held) {
            if (!item.first)
                continue;
            item.first->write(item.second);
            item.first->disconnectFromHost();
        }
    }

    QByteArray id;
    bool holdIdentity = false;
    bool rejectIdentity = false;
    bool rejectPreferences = false;
    bool separateIdentitiesByToken = false;
    bool holdSecondUserPage = false;
    int userPageRequests = 0;
    int pageZeroResponses = 0;
    QVector<QPair<QPointer<QTcpSocket>, QByteArray>> heldSecondPages;
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

        AccountSessionManager sessions;
        Backend& backendOne = sessions.createLoginBackend();
        Backend& backendTwo = sessions.createLoginBackend();
        QCOMPARE(sessions.backendCount(), 2);
        QCOMPARE(sessions.registeredCount(), 0);
        QVERIFY(!sessions.activate(backendOne));
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

        QVERIFY(sessions.registerAuthenticated(backendOne));
        QVERIFY(sessions.registerAuthenticated(backendTwo));
        QVERIFY(sessions.registerAuthenticated(backendOne)); // Idempotent.
        QCOMPARE(sessions.registeredCount(), 2);
        const auto registered = sessions.registeredBackends();
        QCOMPARE(registered.size(), 2);
        QCOMPARE(registered.at(0), &backendOne);
        QCOMPARE(registered.at(1), &backendTwo);
        QCOMPARE(sessions.activeBackend(), &backendOne);
        QCOMPARE(sessions.findAccount(serverOne.origin(), QStringLiteral("user-one")),
                 &backendOne);
        QCOMPARE(sessions.findAccount(serverTwo.origin(), QStringLiteral("user-two")),
                 &backendTwo);
        QCOMPARE(sessions.findAccount(serverOne.origin().chopped(1),
                                      QStringLiteral("user-one")), &backendOne);
        QVERIFY(!sessions.findAccount(serverTwo.origin(), QStringLiteral("user-one")));
        QVERIFY(sessions.activate(backendTwo));
        QCOMPARE(sessions.activeBackend(), &backendTwo);
        QVERIFY(sessions.activate(backendOne));
        QCOMPARE(sessions.activeBackend(), &backendOne);

        // Each Backend's preferences request must use its own server host
        // and authentication, without any ambient global HTTP identity.
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

        // Both accounts perform normal Backend API operations. They must
        // retrieve different server-local profiles, not just use different headers.
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

        // Backend::retrieveFile() previously kept a second, process-wide
        // disk cache keyed only by file ID. It must now use the owning
        // AttachmentService's HTTP cache and never mix equal IDs across origins.
        // A distinct path avoids prewarming this check in the earlier downloads.
        QByteArray firstLegacyFile;
        QByteArray secondLegacyFile;
        backendOne.retrieveFile(QStringLiteral("legacy-file"),
            [&](const QByteArray& data) { firstLegacyFile = data; });
        backendTwo.retrieveFile(QStringLiteral("legacy-file"),
            [&](const QByteArray& data) { secondLegacyFile = data; });
        QTRY_COMPARE(firstLegacyFile, QByteArrayLiteral("bytes-user-one"));
        QTRY_COMPARE(secondLegacyFile, QByteArrayLiteral("bytes-user-two"));
        QCOMPARE(serverOne.fileRequests, 4);
        QCOMPARE(serverTwo.fileRequests, 4);

        // A second read should use each account's own cached content rather
        // than a shared last-writer entry or another network fetch.
        QByteArray firstCachedFile;
        QByteArray secondCachedFile;
        backendOne.retrieveFile(QStringLiteral("legacy-file"),
            [&](const QByteArray& data) { firstCachedFile = data; });
        backendTwo.retrieveFile(QStringLiteral("legacy-file"),
            [&](const QByteArray& data) { secondCachedFile = data; });
        QTRY_COMPARE(firstCachedFile, QByteArrayLiteral("bytes-user-one"));
        QTRY_COMPARE(secondCachedFile, QByteArrayLiteral("bytes-user-two"));
        QCOMPARE(serverOne.fileRequests, 4);
        QCOMPARE(serverTwo.fileRequests, 4);

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

        // Resetting one Backend must leave the other account able to make
        // fresh, authenticated requests. Do not assert a preferences count:
        // other services can independently refresh preferences in the background.
        backendOne.reset();
        QVERIFY(!backendOne.loggedIn());
        QVERIFY(backendTwo.loggedIn());
        // Session-ending invalidates only this registration. The second
        // account stays selected and retains all its Storage and transports.
        QCOMPARE(sessions.registeredCount(), 1);
        QCOMPARE(sessions.registeredBackends().size(), 1);
        QCOMPARE(sessions.registeredBackends().first(), &backendTwo);
        QCOMPARE(sessions.activeBackend(), &backendTwo);
        QVERIFY(!sessions.findAccount(serverOne.origin(), QStringLiteral("user-one")));
        QCOMPARE(sessions.findAccount(serverTwo.origin(), QStringLiteral("user-two")),
                 &backendTwo);
        QVERIFY(!sessions.registerAuthenticated(backendOne));
        QString profileAfterOtherReset;
        backendTwo.retrieveUser(QStringLiteral("after-other-backend-reset"),
                                [&](const BackendUser& user) {
            profileAfterOtherReset = user.id;
        });
        QTRY_COMPARE(profileAfterOtherReset, QStringLiteral("user-two-profile"));
        checkCookies(serverTwo.profileCookies, "token-two", "token-one");
    }

    void userPageCompletionIsIndependentAcrossTwoServers()
    {
        FakeMattermostServer firstServer(QByteArrayLiteral("paged-one"));
        FakeMattermostServer secondServer(QByteArrayLiteral("paged-two"));
        firstServer.holdSecondUserPage = true;
        secondServer.holdSecondUserPage = true;
        QVERIFY(firstServer.listen(QHostAddress::LocalHost));
        QVERIFY(secondServer.listen(QHostAddress::LocalHost));

        Backend first;
        Backend second;
        bool firstReady = false;
        bool secondReady = false;
        first.login(credentials(firstServer, QStringLiteral("first-token")),
                    [&](const QString& token) { firstReady = !token.isEmpty(); });
        second.login(credentials(secondServer, QStringLiteral("second-token")),
                     [&](const QString& token) { secondReady = !token.isEmpty(); });
        QTRY_VERIFY(firstReady && secondReady);
        first.getStorage().totalUsersCount = 200;
        second.getStorage().totalUsersCount = 200;
        QSignalSpy firstDone(&first, &Backend::onAllUsers);
        QSignalSpy secondDone(&second, &Backend::onAllUsers);
        QVERIFY(firstDone.isValid());
        QVERIFY(secondDone.isValid());

        first.retrieveAllUsers();
        second.retrieveAllUsers();
        // Wait for both first-page callbacks to execute before allowing
        // either server's second page. The old static counter would report
        // completion after the two first pages, regardless of account.
        QTRY_VERIFY(first.getStorage().getUserById(
            QStringLiteral("paged-one-page-zero")) != nullptr);
        QTRY_VERIFY(second.getStorage().getUserById(
            QStringLiteral("paged-two-page-zero")) != nullptr);
        QTRY_COMPARE(firstServer.heldSecondPages.size(), 1);
        QTRY_COMPARE(secondServer.heldSecondPages.size(), 1);
        QCOMPARE(firstDone.count(), 0);
        QCOMPARE(secondDone.count(), 0);

        firstServer.releaseSecondUserPages();
        secondServer.releaseSecondUserPages();
        QTRY_COMPARE(firstDone.count(), 1);
        QTRY_COMPARE(secondDone.count(), 1);
        QVERIFY(first.getStorage().getUserById(
            QStringLiteral("paged-one-page-one")) != nullptr);
        QVERIFY(second.getStorage().getUserById(
            QStringLiteral("paged-two-page-one")) != nullptr);
        QVERIFY(!first.getStorage().getUserById(
            QStringLiteral("paged-two-page-one")));
        QVERIFY(!second.getStorage().getUserById(
            QStringLiteral("paged-one-page-one")));
    }

    void twoUsersOnSameServerHaveSeparateBackendIdentities()
    {
        FakeMattermostServer server(QByteArrayLiteral("shared-server"));
        server.separateIdentitiesByToken = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));

        AccountSessionManager sessions;
        Backend& first = sessions.createLoginBackend();
        Backend& second = sessions.createLoginBackend();
        bool firstReady = false;
        bool secondReady = false;
        first.login(credentials(server, QStringLiteral("token-one")),
                    [&](const QString& token) { firstReady = !token.isEmpty(); });
        second.login(credentials(server, QStringLiteral("token-two")),
                     [&](const QString& token) { secondReady = !token.isEmpty(); });
        QTRY_VERIFY(firstReady && secondReady);
        QVERIFY(sessions.registerAuthenticated(first));
        QVERIFY(sessions.registerAuthenticated(second));
        QCOMPARE(sessions.registeredCount(), 2);
        QCOMPARE(sessions.findAccount(server.origin(), QStringLiteral("shared-server-one")),
                 &first);
        QCOMPARE(sessions.findAccount(server.origin(), QStringLiteral("shared-server-two")),
                 &second);

        QVERIFY(sessions.activate(second));
        QCOMPARE(sessions.activeBackend(), &second);
        QVERIFY(sessions.activate(first));
        QCOMPARE(sessions.activeBackend(), &first);

        // The first account's logout must not drop the second account's
        // authentication, HTTP connection state or registration.
        first.reset();
        QCOMPARE(sessions.registeredCount(), 1);
        QCOMPARE(sessions.activeBackend(), &second);
        QVERIFY(second.loggedIn());
        QVERIFY(!sessions.findAccount(server.origin(),
                                     QStringLiteral("shared-server-one")));
        second.retrieveUserPreferences();
        QTRY_VERIFY(server.preferencesRequests >= 3);
        // The per-request cookie lists must include both independent tokens,
        // not a single shared last-login token.
        bool sawOne = false;
        bool sawTwo = false;
        for (const QByteArray& cookie : server.preferenceCookies) {
            const bool one = cookie.contains("MMAUTHTOKEN=token-one");
            const bool two = cookie.contains("MMAUTHTOKEN=token-two");
            QVERIFY(one != two);
            sawOne = sawOne || one;
            sawTwo = sawTwo || two;
        }
        QVERIFY(sawOne && sawTwo);
    }

    void managerOwnsBackendsThroughQObjectParentage()
    {
        QPointer<Backend> firstGuard;
        QPointer<Backend> secondGuard;
        {
            AccountSessionManager accounts;
            Backend& first = accounts.createLoginBackend();
            Backend& second = accounts.createLoginBackend();
            firstGuard = &first;
            secondGuard = &second;
            QCOMPARE(first.parent(), static_cast<QObject*>(&accounts));
            QCOMPARE(second.parent(), static_cast<QObject*>(&accounts));
            QCOMPARE(accounts.backendCount(), 2);

            QSignalSpy firstDestroyed(&first, &QObject::destroyed);
            QVERIFY(firstDestroyed.isValid());
            QVERIFY(accounts.removeBackend(first));
            QCOMPARE(firstDestroyed.count(), 1);
            QVERIFY(firstGuard.isNull());
            QVERIFY(!secondGuard.isNull());
            QCOMPARE(accounts.backendCount(), 1);
            QVERIFY(accounts.activeBackend() == nullptr);
        }
        // The surviving Backend has exactly one lifetime owner. QObject
        // deletes it with its AccountSessionManager, without any unique_ptr.
        QVERIFY(secondGuard.isNull());
    }

    void duplicateSessionRegistrationCannotReplaceAnExistingAccount()
    {
        FakeMattermostServer server(QByteArrayLiteral("same-user"));
        QVERIFY(server.listen(QHostAddress::LocalHost));

        AccountSessionManager sessions;
        Backend& first = sessions.createLoginBackend();
        Backend& duplicate = sessions.createLoginBackend();
        bool firstReady = false;
        bool duplicateReady = false;
        first.login(credentials(server, QStringLiteral("one-token")),
                    [&](const QString& token) {
            firstReady = !token.isEmpty();
        });
        duplicate.login(credentials(server, QStringLiteral("two-token")),
                        [&](const QString& token) {
            duplicateReady = !token.isEmpty();
        });
        QTRY_VERIFY(firstReady && duplicateReady);

        QVERIFY(sessions.registerAuthenticated(first));
        QVERIFY(!sessions.registerAuthenticated(duplicate));
        QCOMPARE(sessions.registeredCount(), 1);
        QCOMPARE(sessions.activeBackend(), &first);
        QCOMPARE(sessions.findAccount(server.origin(), QStringLiteral("same-user")),
                 &first);
        QVERIFY(!sessions.activate(duplicate));

        // Releasing the first authenticated session makes its identity
        // available to the other Backend without recreating it or leaking
        // the first account's view/transport into the second.
        first.reset();
        QCOMPARE(sessions.registeredCount(), 0);
        QVERIFY(sessions.activeBackend() == nullptr);
        QVERIFY(sessions.registerAuthenticated(duplicate));
        QCOMPARE(sessions.activeBackend(), &duplicate);
        QCOMPARE(sessions.registeredCount(), 1);
        QCOMPARE(sessions.findAccount(server.origin(), QStringLiteral("same-user")),
                 &duplicate);
        QVERIFY(sessions.removeBackend(first));
        QCOMPARE(sessions.backendCount(), 1);
    }

    void savedAttachmentPathsDoNotSurviveAccountSwitch()
    {
        FakeMattermostServer firstServer(QByteArrayLiteral("old-account"));
        FakeMattermostServer secondServer(QByteArrayLiteral("new-account"));
        QVERIFY(firstServer.listen(QHostAddress::LocalHost));
        QVERIFY(secondServer.listen(QHostAddress::LocalHost));

        Backend backend;
        bool firstReady = false;
        backend.login(credentials(firstServer, QStringLiteral("old-token")),
                      [&](const QString& token) { firstReady = !token.isEmpty(); });
        QTRY_VERIFY(firstReady);

        QTemporaryDir local;
        QVERIFY(local.isValid());
        const QString savedPath = local.filePath(QStringLiteral("saved.bin"));
        QFile saved(savedPath);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write(QByteArrayLiteral("old-account-private-bytes")),
                 qint64(QByteArrayLiteral("old-account-private-bytes").size()));
        saved.close();

        AttachmentService& attachments = AttachmentService::instance(backend);
        attachments.rememberLocalFile(QStringLiteral("shared-file"), savedPath);
        QCOMPARE(attachments.localFilePath(QStringLiteral("shared-file")),
                 savedPath);

        backend.reset();
        QVERIFY(attachments.localFilePath(QStringLiteral("shared-file")).isEmpty());

        bool secondReady = false;
        backend.login(credentials(secondServer, QStringLiteral("new-token")),
                      [&](const QString& token) { secondReady = !token.isEmpty(); });
        QTRY_VERIFY(secondReady);
        // Backend remains the same QObject across both authenticated sessions;
        // a new server must not inherit previously saved local file paths.
        QVERIFY(attachments.localFilePath(QStringLiteral("shared-file")).isEmpty());
        QByteArray secondData;
        backend.retrieveFile(QStringLiteral("shared-file"),
                             [&](const QByteArray& data) { secondData = data; });
        QTRY_COMPARE(secondData, QByteArrayLiteral("bytes-new-account"));
        QCOMPARE(firstServer.fileRequests, 0);
        QCOMPARE(secondServer.fileRequests, 1);
        checkCookies(secondServer.fileCookies,
                     "new-token", "old-token");
    }

    void unauthorizedEndpointValidatesOnlyItsOwnAccount()
    {
        FakeMattermostServer firstServer(QByteArrayLiteral("first-user"));
        FakeMattermostServer secondServer(QByteArrayLiteral("second-user"));
        QVERIFY(firstServer.listen(QHostAddress::LocalHost));
        QVERIFY(secondServer.listen(QHostAddress::LocalHost));

        Backend first;
        Backend second;
        bool firstReady = false;
        bool secondReady = false;
        first.login(credentials(firstServer, QStringLiteral("first-token")),
                    [&](const QString& token) { firstReady = !token.isEmpty(); });
        second.login(credentials(secondServer, QStringLiteral("second-token")),
                     [&](const QString& token) { secondReady = !token.isEmpty(); });
        QTRY_VERIFY(firstReady && secondReady);
        QSignalSpy firstExpired(&first, &Backend::authenticationExpired);
        QSignalSpy secondExpired(&second, &Backend::authenticationExpired);
        QVERIFY(firstExpired.isValid());
        QVERIFY(secondExpired.isValid());

        // An endpoint-specific 401 alone is insufficient to invalidate the
        // session if /users/me still accepts this account's token.
        firstServer.rejectPreferences = true;
        first.retrieveUserPreferences();
        QTRY_COMPARE(firstServer.identityRequests, 2);
        QCOMPARE(firstExpired.count(), 0);
        QVERIFY(first.loggedIn());
        QVERIFY(second.loggedIn());

        // The same server subsequently rejects /users/me as well. Emit just
        // one account-local expiration notification; do not kill the process.
        firstServer.rejectIdentity = true;
        first.retrieveUserPreferences();
        QTRY_COMPARE(firstExpired.count(), 1);
        QCOMPARE(secondExpired.count(), 0);
        QVERIFY(second.loggedIn());
        const int previousSecondPreferences = secondServer.preferencesRequests;
        second.retrieveUserPreferences();
        QTRY_VERIFY(secondServer.preferencesRequests > previousSecondPreferences);
        checkCookies(secondServer.preferenceCookies,
                     "second-token", "first-token");

        // Further simultaneous errors must not create multiple logout flows.
        first.retrieveUserPreferences();
        QTRY_VERIFY(firstServer.preferencesRequests >= 4);
        QCOMPARE(firstExpired.count(), 1);
    }

    void cancellationOfOneLoginDoesNotCancelTheOther()
    {
        FakeMattermostServer stalled(QByteArrayLiteral("stalled-user"));
        FakeMattermostServer healthy(QByteArrayLiteral("healthy-user"));
        stalled.holdIdentity = true;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QVERIFY(healthy.listen(QHostAddress::LocalHost));
        AccountSessionManager sessions;
        Backend& waitingBackend = sessions.createLoginBackend();
        Backend& activeBackend = sessions.createLoginBackend();

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
        QVERIFY(!sessions.registerAuthenticated(waitingBackend));
        QVERIFY(sessions.registerAuthenticated(activeBackend));
        QCOMPARE(sessions.activeBackend(), &activeBackend);
        QVERIFY(sessions.removeBackend(waitingBackend));
        QCOMPARE(sessions.backendCount(), 1);
        QCOMPARE(sessions.registeredCount(), 1);

        activeBackend.retrieveUserPreferences();
        QTRY_COMPARE(healthy.preferencesRequests, 2);
        checkCookies(healthy.preferenceCookies, "active-token", "stalled-token");
    }
};

QTEST_MAIN(BackendMultiServerLoginTest)
#include "BackendMultiServerLoginTest.moc"
