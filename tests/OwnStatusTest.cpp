#include <QtTest>

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QWidget>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/HTTPConnector.h"
#include "backend/CustomStatusValidation.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "ui/EmojiPresentation.h"
#include "ui/UserStatusEmoji.h"
#include "backend/types/BackendUser.h"
#include "info-dialogs/UserProfileDialog.h"

using namespace Mattermost;

class OwnStatusTest final : public QObject
{
    Q_OBJECT
private slots:
    void emojiOnlyAndEmptyStatusContent()
    {
        QVERIFY(hasCustomStatusContent(QStringLiteral("coffee"), QString()));
        QVERIFY(hasCustomStatusContent(QString(), QStringLiteral("Working")));
        QVERIFY(hasCustomStatusContent(QStringLiteral("coffee"), QStringLiteral("Working")));
        QVERIFY(!hasCustomStatusContent(QString(), QString()));
        QVERIFY(!hasCustomStatusContent(QStringLiteral("  "), QStringLiteral("  ")));
        QVERIFY(isEmojiOnlyCustomStatus(QStringLiteral("coffee"), QString()));
        QVERIFY(isEmojiOnlyCustomStatus(QStringLiteral("coffee"), QStringLiteral("  ")));
        QVERIFY(!isEmojiOnlyCustomStatus(QStringLiteral("coffee"),
                                         QStringLiteral("Working")));
        QVERIFY(!isEmojiOnlyCustomStatus(QString(), QStringLiteral("Working")));
        QVERIFY(!isEmojiOnlyCustomStatus(QStringLiteral("  "), QString()));
    }

    void cachedCustomStatusEmojiAndExpiration()
    {
        const auto profile = [](const QString& expiresAt) {
            const QJsonObject status {
                {QStringLiteral("emoji"), QStringLiteral("coffee")},
                {QStringLiteral("text"), QStringLiteral("In a meeting")},
                {QStringLiteral("expires_at"), expiresAt}
            };
            return QJsonObject {
                {QStringLiteral("id"), QStringLiteral("user-1")},
                {QStringLiteral("username"), QStringLiteral("test-user")},
                {QStringLiteral("props"), QJsonObject {
                    {QStringLiteral("customStatus"), QString::fromUtf8(
                        QJsonDocument(status).toJson(QJsonDocument::Compact))}
                }}
            };
        };

        BackendUser active(profile(QDateTime::currentDateTimeUtc()
                                       .addSecs(3600).toString(Qt::ISODate)));
        QCOMPARE(UserStatusEmoji::name(active), QStringLiteral("coffee"));
        QCOMPARE(UserStatusEmoji::text(active), QStringLiteral("In a meeting"));

        // A decoded customStatus object with no text is also a valid status.
        const QJsonObject emojiOnly {
            {QStringLiteral("emoji"), QStringLiteral("coffee")},
            {QStringLiteral("text"), QString()}
        };
        BackendUser ownUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("me")},
            {QStringLiteral("props"), QJsonObject {
                {QStringLiteral("customStatus"), emojiOnly}
            }}
        });
        QCOMPARE(UserStatusEmoji::name(ownUser), QStringLiteral("coffee"));
        QVERIFY(UserStatusEmoji::text(ownUser).isEmpty());

        // Go time.Time's zero value is serialized for "Don't clear" statuses.
        // It is a valid ISO date, but must not count as an expired status.
        BackendUser neverExpires(profile(QStringLiteral("0001-01-01T00:00:00Z")));
        QCOMPARE(UserStatusEmoji::name(neverExpires), QStringLiteral("coffee"));
        QCOMPARE(UserStatusEmoji::text(neverExpires),
                 QStringLiteral("In a meeting"));
        const QJsonObject zeroTimeStatus {
            {QStringLiteral("emoji"), QStringLiteral("coffee")},
            {QStringLiteral("text"), QString()},
            {QStringLiteral("expires_at"), QStringLiteral("0001-01-01T00:00:00Z")}
        };
        BackendUser emojiOnlyNeverExpires(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("my-user")},
            {QStringLiteral("props"), QJsonObject {
                {QStringLiteral("customStatus"), zeroTimeStatus}
            }}
        });
        QCOMPARE(UserStatusEmoji::name(emojiOnlyNeverExpires),
                 QStringLiteral("coffee"));
        QVERIFY(UserStatusEmoji::text(emojiOnlyNeverExpires).isEmpty());
        EmojiRegistry emojiRegistry;
        QVERIFY(!UserStatusEmoji::pixmap(emojiRegistry, emojiOnlyNeverExpires,
                                         20, QFont()).isNull());

        BackendUser expired(profile(QDateTime::currentDateTimeUtc()
                                        .addSecs(-60).toString(Qt::ISODate)));
        QVERIFY(UserStatusEmoji::name(expired).isEmpty());
        QVERIFY(UserStatusEmoji::text(expired).isEmpty());

        QSignalSpy changed(&active, &BackendUser::onCustomStatusChanged);
        QString details;
        active.updateFrom(expired, details);
        QCOMPARE(changed.count(), 1);
        QVERIFY(UserStatusEmoji::name(active).isEmpty());
    }

    void profileDisplaysCustomStatusAndTracksEdits()
    {
        Backend backend;
        const QJsonObject customStatus {
            {QStringLiteral("emoji"), QStringLiteral("coffee")},
            {QStringLiteral("text"), QStringLiteral("<focus & work>")},
            {QStringLiteral("expires_at"), QStringLiteral("0001-01-01T00:00:00Z")}
        };
        BackendUser* user = backend.getStorage().addUser(
            QJsonObject {{QStringLiteral("id"), QStringLiteral("profile-user")},
                         {QStringLiteral("username"), QStringLiteral("test-user")},
                         {QStringLiteral("props"), QJsonObject {
                             {QStringLiteral("customStatus"), customStatus}
                         }}},
            true);
        QVERIFY(user);
        QCOMPARE(backend.getStorage().loginUser, user);
        // Prevent unrelated avatar HTTP work from the profile dialog.
        user->avatar = QPixmap(16, 16);
        user->avatar.fill(Qt::red);
        user->avatar_picture_update = user->last_picture_update;
        user->status = QStringLiteral("away");

        UserProfileDialog dialog(backend, *user);
        auto* customRow = dialog.findChild<QWidget*>(QStringLiteral("customStatusRow"));
        auto* customText = dialog.findChild<QLabel*>(QStringLiteral("customStatusText"));
        auto* customEmoji = dialog.findChild<QLabel*>(QStringLiteral("customStatusEmoji"));
        auto* presence = dialog.findChild<QLabel*>(QStringLiteral("statusValue"));
        QVERIFY(customRow);
        QVERIFY(customText);
        QVERIFY(customEmoji);
        QVERIFY(presence);
        dialog.setModal(true);
        dialog.show();
        QCoreApplication::processEvents();
        QVERIFY(customRow->isVisible());
        QVERIFY(customEmoji->isVisible());
        QVERIFY(!customEmoji->pixmap(Qt::ReturnByValue).isNull());
        QCOMPARE(customText->text(), QStringLiteral("<focus & work>"));
        QCOMPARE(customText->textFormat(), Qt::PlainText);
        QCOMPARE(presence->text(), QStringLiteral("away"));

        user->props.customStatus.text = QStringLiteral("Meeting");
        emit user->onCustomStatusChanged();
        QCOMPARE(customText->text(), QStringLiteral("Meeting"));

        user->props.customStatus.text.clear();
        emit user->onCustomStatusChanged();
        QVERIFY(customRow->isVisible()); // Emoji-only status remains visible.
        QVERIFY(customEmoji->isVisible());
        QVERIFY(customText->isHidden());
        QVERIFY(!customEmoji->pixmap(Qt::ReturnByValue).isNull());

        // Opening a fresh profile while the saved status has only an emoji
        // must work too, not just updates to a dialog that showed text first.
        UserProfileDialog emojiOnlyDialog(backend, *user);
        emojiOnlyDialog.setModal(true);
        emojiOnlyDialog.show();
        QCoreApplication::processEvents();
        auto* freshRow = emojiOnlyDialog.findChild<QWidget*>(
            QStringLiteral("customStatusRow"));
        auto* freshEmoji = emojiOnlyDialog.findChild<QLabel*>(
            QStringLiteral("customStatusEmoji"));
        QVERIFY(freshRow && freshRow->isVisible());
        QVERIFY(freshEmoji && freshEmoji->isVisible());
        QVERIFY(!freshEmoji->pixmap(Qt::ReturnByValue).isNull());

        user->props.customStatus = {};
        emit user->onCustomStatusChanged();
        QVERIFY(customRow->isHidden());
        QCOMPARE(presence->text(), QStringLiteral("away"));
    }

    void emojiOnlyStatusHasTripleSize()
    {
        QFont standard;
        standard.setPointSize(10);
        const QFont large = EmojiPresentation::fontForMode(
            standard, EmojiPresentation::Mode::StatusJumbo);
        QCOMPARE(large.pointSizeF(), 3.0 * standard.pointSizeF());
        QCOMPARE(EmojiPresentation::extent(
                     standard, EmojiPresentation::Mode::Reaction), 16);
        QCOMPARE(EmojiPresentation::extent(
                     standard, EmojiPresentation::Mode::StatusJumbo), 48);
    }

    void reconnectWithMultiplePendingReplies()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        QHash<QByteArray, int> attempts;
        bool restarting = false;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray line = buffer->left(
                        buffer->indexOf("\r\n"));
                    const QByteArrayList parts = line.split(' ');
                    if (parts.size() < 2 || parts.at(0) != "GET") return;
                    ++attempts[parts.at(1)];

                    if (!restarting) {
                        return;
                    }

                    const QByteArray body = QByteArrayLiteral("{\"ok\":true}");
                    socket->write(QByteArrayLiteral(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: ") + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });

        HTTPConnector first, second;
        int completed = 0;
        bool allSucceeded = true;
        for (int i = 0; i < 4; ++i) {
            NetworkRequest request(QStringLiteral("probe/%1").arg(i));
            auto callback = HttpResponseCallback(
                [&](const QJsonDocument& doc, const QNetworkReply& reply) {
                    allSucceeded = allSucceeded
                        && reply.error() == QNetworkReply::NoError
                        && doc.object().value(QStringLiteral("ok")).toBool();
                    ++completed;
                });
            if (i % 2 == 0) {
                first.get(request, callback);
            } else {
                second.get(request, callback);
            }
        }

        // All four QNetworkReply objects exist already, even when Qt has
        // not yet dispatched all of their HTTP/1.1 requests to the server.
        // Do not couple the restart trigger to the server's arrival order.
        QTimer::singleShot(200, &server, [&] {
            restarting = true;
            HTTPConnector::restartAllTransports();
        });

        QTRY_COMPARE_WITH_TIMEOUT(completed, 4, 10000);
        QVERIFY(allSucceeded);
        QCOMPARE(attempts.size(), 4);
        for (int i = 0; i < 4; ++i) {
            // Qt can queue requests behind an HTTP/1.1 connection, so only
            // requests that reached the mock server before restart are seen
            // twice. Every logical GET must still finish exactly once.
            const int count = attempts.value(
                QByteArrayLiteral("/api/v4/probe/")
                    + QByteArray::number(i));
            QVERIFY(count >= 1 && count <= 2);
        }
    }

    void canceledStatusMutationIsSilent_data()
    {
        QTest::addColumn<QString>("action");
        QTest::newRow("availability") << QStringLiteral("availability");
        QTest::newRow("custom-set") << QStringLiteral("set");
        QTest::newRow("custom-clear") << QStringLiteral("clear");
    }

    void canceledStatusMutationIsSilent()
    {
        QFETCH(QString, action);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        int incomingMutations = 0;
        bool restartScheduled = false;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int headersEnd = buffer->indexOf("\r\n\r\n");
                    if (headersEnd < 0 || restartScheduled) return;
                    const QByteArray headers = buffer->left(headersEnd);
                    int contentLength = 0;
                    for (const QByteArray& header : headers.split('\n')) {
                        if (header.trimmed().toLower().startsWith("content-length:")) {
                            contentLength = header.mid(header.indexOf(':') + 1)
                                                .trimmed().toInt();
                        }
                    }
                    if (buffer->size() < headersEnd + 4 + contentLength) return;

                    ++incomingMutations;
                    restartScheduled = true;
                    // Mimic a route change after the server received a
                    // mutation but before any HTTP response. No success or
                    // failure can be inferred from the canceled reply.
                    QTimer::singleShot(0, &server, [] {
                        HTTPConnector::restartAllTransports();
                    });
                });
            }
        });

        Backend backend;
        BackendUser* user = backend.getStorage().addUser(
            QJsonObject {{QStringLiteral("id"), QStringLiteral("user-1")},
                         {QStringLiteral("username"), QStringLiteral("user")}},
            true);
        QVERIFY(user);
        user->status = QStringLiteral("offline");
        QSignalSpy availabilitySpy(user, &BackendUser::onStatusChanged);
        QSignalSpy customSpy(&backend, &Backend::onOwnCustomStatusChanged);

        bool finished = false;
        bool succeeded = true;
        QString reason;
        auto callback = [&](bool ok, const QString& error) {
            succeeded = ok;
            reason = error;
            finished = true;
        };
        if (action == QLatin1String("availability")) {
            backend.setOwnAvailability(QStringLiteral("online"), callback);
        } else if (action == QLatin1String("set")) {
            backend.setOwnCustomStatus(QStringLiteral("coffee"),
                QStringLiteral("Busy"), QString(), QString(), callback);
        } else {
            backend.clearOwnCustomStatus(callback);
        }

        QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        QVERIFY(!succeeded);
        QVERIFY2(reason.isEmpty(), qPrintable(reason));
        QCOMPARE(incomingMutations, 1);
        QCOMPARE(user->status, QStringLiteral("offline"));
        QCOMPARE(availabilitySpy.size(), 0);
        QCOMPARE(customSpy.size(), 0);
    }

    void statusEndpointsAndFailedUpdate()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        int statusCode = 200;
        QByteArray lastMethod, lastPath, lastBody;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray headers = buffer->left(end);
                    int contentLength = 0;
                    for (const QByteArray& header : headers.split('\n')) {
                        if (header.trimmed().toLower().startsWith("content-length:")) {
                            contentLength = header.mid(header.indexOf(':') + 1)
                                                .trimmed().toInt();
                        }
                    }
                    if (buffer->size() < end + 4 + contentLength) return;
                    const QList<QByteArray> requestParts = headers.split(' ');
                    if (requestParts.size() >= 2) {
                        lastMethod = requestParts.at(0);
                        lastPath = requestParts.at(1);
                    }
                    lastBody = buffer->mid(end + 4, contentLength);
                    socket->write(
                        QByteArray("HTTP/1.1 ") + QByteArray::number(statusCode)
                        + (statusCode == 200 ? " OK\r\n" : " Internal Server Error\r\n")
                        + "Content-Type: application/json\r\n"
                          "Content-Length: 2\r\nConnection: close\r\n\r\n{}");
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });

        Backend backend;
        BackendUser* user = backend.getStorage().addUser(
            QJsonObject {{QStringLiteral("id"), QStringLiteral("user-1")},
                         {QStringLiteral("username"), QStringLiteral("user")}},
            true);
        QVERIFY(user);
        user->status = QStringLiteral("online");
        QSignalSpy availabilitySignal(user, &BackendUser::onStatusChanged);
        QSignalSpy customSignal(&backend, &Backend::onOwnCustomStatusChanged);

        bool finished = false, succeeded = false;
        backend.setOwnAvailability(QStringLiteral("away"),
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("PUT"));
        QVERIFY(lastPath.endsWith("/users/user-1/status"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("away"));
        QCOMPARE(user->status, QStringLiteral("away"));
        QCOMPARE(availabilitySignal.size(), 1);

        finished = false;
        backend.setOwnCustomStatus(QStringLiteral("coffee"),
            QStringLiteral("Concentrating"), QStringLiteral("one_hour"), {},
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("PUT"));
        QVERIFY(lastPath.endsWith("/users/user-1/status/custom"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("duration")).toString(),
                 QStringLiteral("one_hour"));
        QCOMPARE(user->props.customStatus.text, QStringLiteral("Concentrating"));
        QCOMPARE(customSignal.size(), 1);

        // Mattermost allows a status with just an emoji and no text.
        finished = false;
        backend.setOwnCustomStatus(QStringLiteral("coffee"), QString(),
            QStringLiteral("one_hour"), {},
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("PUT"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("coffee"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("text")).toString(), QString());
        QCOMPARE(user->props.customStatus.emoji, QStringLiteral("coffee"));
        QVERIFY(user->props.customStatus.text.isEmpty());
        QCOMPARE(customSignal.size(), 2);

        statusCode = 500;
        finished = false;
        QString failureReason;
        backend.setOwnAvailability(QStringLiteral("dnd"),
            [&](bool ok, const QString& error) {
                succeeded = ok;
                failureReason = error;
                finished = true;
            });
        QTRY_VERIFY(finished);
        QVERIFY(!succeeded);
        QVERIFY(!failureReason.isEmpty()); // Real server errors are reportable.
        QCOMPARE(user->status, QStringLiteral("away"));
        QCOMPARE(availabilitySignal.size(), 1);

        statusCode = 200;
        finished = false;
        backend.clearOwnCustomStatus(
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("DELETE"));
        QVERIFY(lastPath.endsWith("/users/user-1/status/custom"));
        QVERIFY(user->props.customStatus.text.isEmpty());
        QCOMPARE(customSignal.size(), 3);
        QVERIFY(user->props.customStatus.emoji.isEmpty());
    }
};

QTEST_MAIN(OwnStatusTest)
#include "OwnStatusTest.moc"
