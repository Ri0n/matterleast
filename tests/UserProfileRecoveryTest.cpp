#include <QtTest>

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextBrowser>
#include <QTextCursor>
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
#include "chat-area/post/MessageContentWidget.h"
#include "info-dialogs/UserProfileDialog.h"

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
    void ownEmojiOnlyCustomStatusAppearsOnPostHeader()
    {
        Backend backend;
        const QJsonObject customStatus {
            {QStringLiteral("emoji"), QStringLiteral("coffee")},
            {QStringLiteral("text"), QString()},
            {QStringLiteral("expires_at"), QStringLiteral("0001-01-01T00:00:00Z")}
        };
        QJsonObject ownProfile = userJson();
        ownProfile.insert(QStringLiteral("props"), QJsonObject {
            {QStringLiteral("customStatus"), QString::fromUtf8(
                QJsonDocument(customStatus).toJson(QJsonDocument::Compact))}
        });
        BackendUser* user = backend.getStorage().addUser(ownProfile, true);
        QVERIFY(user);
        user->status = QStringLiteral("online");
        user->avatar = QPixmap(24, 24);
        user->avatar.fill(Qt::blue);
        user->avatar_picture_update = user->last_picture_update;

        auto post = makePost(backend.getStorage());
        QCOMPARE(post.author, user);
        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        widget.resize(650, 160);
        widget.show();
        QCoreApplication::processEvents();

        auto* emoji = widget.findChild<QLabel*>(
            QStringLiteral("authorCustomStatusEmoji"));
        QVERIFY(emoji);
        QVERIFY(emoji->isVisible());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QVERIFY(!emoji->pixmap().isNull());
#else
        QVERIFY(!emoji->pixmap(Qt::ReturnByValue).isNull());
#endif
        user->props.customStatus = {};
        emit user->onCustomStatusChanged();
        QVERIFY(emoji->isHidden());
    }

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
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QVERIFY(!avatar->pixmap().isNull());
#else
        QVERIFY(!avatar->pixmap(Qt::ReturnByValue).isNull());
#endif

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

    void dottedMentionsRemainClickableAfterBodyRebuild()
    {
        Backend backend;
        BackendUser* author = backend.getStorage().addUser(userJson());
        QVERIFY(author);
        auto post = BackendPost(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("aaaaabbbbbcccccdddddeeeee2")},
            {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
            {QStringLiteral("user_id"), kUserId},
            {QStringLiteral("create_at"), 1791534000000.0},
            {QStringLiteral("message"),
             QStringLiteral("Hello @retried.author and @second.user.")},
        }, backend.getStorage());

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        widget.resize(700, 220);
        widget.show();
        QCoreApplication::processEvents();

        auto* content = widget.findChild<MessageContentWidget*>();
        QVERIFY(content);
        auto* firstBrowser = content->findChild<QTextBrowser*>(
            QStringLiteral("messageRichText"));
        QVERIFY(firstBrowser);

        // An attachment/emoji/font refresh recreates all QTextBrowser children.
        // Mention formatting and click handlers must be restored every time.
        content->setInlineAttachmentContext({}, {});
        auto* browser = content->findChild<QTextBrowser*>(
            QStringLiteral("messageRichText"));
        QVERIFY(browser);
        for (const QString& username : {QStringLiteral("retried.author"),
                                        QStringLiteral("second.user")}) {
            QTextCursor mention = browser->document()->find(
                QLatin1Char('@') + username);
            QVERIFY(!mention.isNull());
            mention.setPosition(mention.selectionStart() + 2);
            QVERIFY(mention.charFormat().isAnchor());
            QCOMPARE(mention.charFormat().anchorHref(),
                     QStringLiteral("mattermost-user:///") + username);
        }

        // The sender is already in Storage, so clicking its name must open a
        // profile immediately without waiting for another server lookup.
        QTextCursor senderMention = browser->document()->find(
            QStringLiteral("@retried.author"));
        QVERIFY(!senderMention.isNull());
        const int begin = senderMention.selectionStart();
        const auto mentionPoint = [browser, begin] {
            QTextCursor cursor(browser->document());
            cursor.setPosition(begin + 3);
            const QRect characterStart = browser->cursorRect(cursor);
            cursor.setPosition(begin + 4);
            const QRect characterEnd = browser->cursorRect(cursor);
            // The midpoint of two carets is inside the link glyph; compute
            // it afresh after Qt's queued geometry/layout passes.
            return QPoint(
                (characterStart.center().x() + characterEnd.center().x()) / 2,
                characterStart.center().y());
        };
        // setInlineAttachmentContext() just rebuilt the rich-text controls.
        // Their document and viewport geometry can still be pending.
        QTRY_VERIFY_WITH_TIMEOUT(browser->viewport()->isVisible(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            browser->viewport()->rect().contains(mentionPoint()), 2000);
        const QString expectedHref =
            QStringLiteral("mattermost-user:///retried.author");
        QTRY_COMPARE_WITH_TIMEOUT(browser->anchorAt(mentionPoint()),
                                  expectedHref, 2000);
        // In the offscreen Qt platform a synthetic mouse click can be
        // consumed by QTextEdit's selection machinery without emitting
        // anchorClicked. Assert the viewport hit-test above, then exercise
        // the actual production link-click signal/slot wiring separately.
        // This catches dropped connections on rebuilt QTextBrowser objects.
        QSignalSpy clickSpy(browser, &QTextBrowser::anchorClicked);
        emit browser->anchorClicked(QUrl(browser->anchorAt(mentionPoint())));
        QCOMPARE(clickSpy.count(), 1);
        QTRY_VERIFY(widget.findChild<UserProfileDialog*>());
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
