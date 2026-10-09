#include <QtTest>

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "chat-area/post/PostWidget.h"

using namespace Mattermost;

namespace {

const QString kUserId = QStringLiteral("profile-recovery-user-id");

QJsonObject userJson()
{
    return QJsonObject {
        {QStringLiteral("id"), kUserId},
        {QStringLiteral("username"), QStringLiteral("retried.author")},
        {QStringLiteral("first_name"), QStringLiteral("Resolved")},
        {QStringLiteral("last_name"), QStringLiteral("Author")},
        {QStringLiteral("last_picture_update"), 1},
    };
}

class ProfileServer : public QTcpServer {
public:
    int requests = 0;
    bool failFirst = true;

    ProfileServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QTcpSocket::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray bytes = socket->property("httpBuffer").toByteArray();
                    bytes += socket->readAll();
                    socket->setProperty("httpBuffer", bytes);
                    if (!bytes.contains("\r\n\r\n")
                        || socket->property("replied").toBool()) {
                        return;
                    }
                    socket->setProperty("replied", true);
                    ++requests;
                    const bool fail = failFirst && requests == 1;
                    const QByteArray body = fail
                        ? QByteArrayLiteral("temporary error")
                        : QJsonDocument(QJsonArray { userJson() }).toJson(QJsonDocument::Compact);
                    const QByteArray status = fail
                        ? QByteArrayLiteral("503 Service Unavailable")
                        : QByteArrayLiteral("200 OK");
                    socket->write("HTTP/1.1 " + status
                        + "\r\nContent-Type: application/json"
                        + "\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size())
                        + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

BackendPost makePost(Storage& storage)
{
    return BackendPost(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("aaaaabbbbbcccccdddddeeeee1")},
        {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
        {QStringLiteral("user_id"), kUserId},
        {QStringLiteral("create_at"), 1791534000000.0},
        {QStringLiteral("message"), QStringLiteral("A message from this user")},
    }, storage);
}

} // namespace

class UserProfileRecoveryTest : public QObject {
    Q_OBJECT

private slots:
    void lateProfileUpdatesAlreadyVisibleAuthor()
    {
        Backend backend;
        auto post = makePost(backend.getStorage());
        QCOMPARE(post.author, nullptr);

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        auto* label = widget.findChild<QLabel*>(QStringLiteral("authorName"));
        QVERIFY(label);
        QCOMPARE(label->text(), kUserId);

        BackendUser* user = backend.getStorage().addUser(userJson());
        QVERIFY(user);
        QPixmap picture(24, 24);
        picture.fill(Qt::blue);
        user->avatar = picture;
        user->avatar_picture_update = user->last_picture_update;
        auto& profiles = UserProfileService::instance(backend);

        emit profiles.profileResolved(kUserId);

        QCOMPARE(post.author, user);
        QCOMPARE(label->text(), QStringLiteral("Resolved Author"));
        auto* avatar = widget.findChild<QLabel*>(QStringLiteral("authorAvatar"));
        QVERIFY(avatar);
        QVERIFY(!avatar->pixmap()->isNull());

        // The same widget must also update if a later profile refresh renames
        // an already known user (without changing the post identity).
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), kUserId},
            {QStringLiteral("username"), QStringLiteral("retried.author")},
            {QStringLiteral("first_name"), QStringLiteral("Updated")},
            {QStringLiteral("last_name"), QStringLiteral("Author")},
            {QStringLiteral("last_picture_update"), 1},
        });
        emit profiles.profileResolved(kUserId);
        QCOMPARE(label->text(), QStringLiteral("Updated Author"));
    }

    void temporaryProfileHttpFailureRetriesAndResolvesWaiter()
    {
        ProfileServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));

        Backend backend;
        auto& profiles = UserProfileService::instance(backend);
        int callbackCount = 0;
        const BackendUser* resolved = nullptr;
        QSignalSpy arrivals(&profiles, &UserProfileService::profileResolved);

        profiles.ensureUser(kUserId, [&](const BackendUser* user) {
            ++callbackCount;
            resolved = user;
        });

        QTRY_VERIFY_WITH_TIMEOUT(server.requests >= 2, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(callbackCount, 1, 10000);
        QVERIFY(resolved);
        QCOMPARE(resolved->getDisplayName(), QStringLiteral("Resolved Author"));
        QCOMPARE(arrivals.size(), 1);
        QCOMPARE(backend.getStorage().getUserById(kUserId), resolved);
    }
};

QTEST_MAIN(UserProfileRecoveryTest)
#include "UserProfileRecoveryTest.moc"
