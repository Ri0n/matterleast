#include <QtTest>

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

#include "Settings.h"
#include "options/MLOptions.h"
#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/UserProfileService.h"
#include "chat-area/post/ForwardPostDialog.h"
#include "chat-area/post/PostWidget.h"
#include "chat-area/QuotedPostPreview.h"
#include "chat-area/post/PostPermalinkService.h"
#include "navigation/MattermostUrlRouter.h"

using namespace Mattermost;

class ForwardPostDialogTest : public QObject
{
    Q_OBJECT

private:
    static BackendChannel* addChannel(Backend& backend, const QString& id,
                                      const QString& type,
                                      const QString& name)
    {
        Storage& storage = backend.getStorage();
        BackendTeam* team = storage.getTeamById(QStringLiteral("team"));
        if (!team) {
            team = storage.addTeam(QJsonObject{
                {QStringLiteral("id"), QStringLiteral("team")},
                {QStringLiteral("name"), QStringLiteral("team")},
                {QStringLiteral("display_name"), QStringLiteral("Team")},
            });
        }
        return storage.addTeamChannel(*team, QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("team_id"), QStringLiteral("team")},
            {QStringLiteral("type"), type},
            {QStringLiteral("name"), name},
            {QStringLiteral("display_name"), name},
        });
    }

    static BackendPost sourcePost(Backend& backend, const QString& channelId)
    {
        return BackendPost(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("original-post")},
            {QStringLiteral("channel_id"), channelId},
            {QStringLiteral("message"), QStringLiteral("Original content")},
        }, backend.getStorage());
    }

private slots:
    void initTestCase()
    {
        NetworkRequest::setHost(QStringLiteral("https://mattermost.example.test/"));
    }

    void cleanup()
    {
        MLOptions::instance()->optionObject<bool>(
            FORWARD_WITH_PERMALINK, FORWARD_WITH_PERMALINK_DEFAULT)
            ->setValue(FORWARD_WITH_PERMALINK_DEFAULT);
    }

    void copyLinkStillUsesTeamIndependentRedirect()
    {
        const QString postId = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
        const QString url = PostPermalinkService::redirectPermalink(postId);
        QCOMPARE(url,
                 QStringLiteral("https://mattermost.example.test/_redirect/pl/")
                     + postId);
        const MattermostUrlRoute route = MattermostUrlRouter::classify(
            QUrl(url), QUrl(NetworkRequest::host()));
        QCOMPARE(route.kind, MattermostUrlRoute::Kind::Post);
        QCOMPARE(route.target, postId);
        QVERIFY(PostPermalinkService::redirectPermalink({}).isEmpty());
    }

    void publicOriginListsKnownDestinationsAndFilters()
    {
        Backend backend;
        QVERIFY(addChannel(backend, QStringLiteral("source"), QStringLiteral("O"),
                           QStringLiteral("source")));
        QVERIFY(addChannel(backend, QStringLiteral("destination"), QStringLiteral("O"),
                           QStringLiteral("destination")));
        QVERIFY(addChannel(backend, QStringLiteral("private"), QStringLiteral("P"),
                           QStringLiteral("private")));
        ForwardPostDialog dialog(backend, sourcePost(backend, QStringLiteral("source")));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("forwardDestinations"));
        auto* filter = dialog.findChild<QLineEdit*>(QStringLiteral("forwardDestinationFilter"));
        auto* submit = dialog.findChild<QPushButton*>(QStringLiteral("forwardSubmit"));
        QVERIFY(list && filter && submit);
        QCOMPARE(list->count(), 3);
        QVERIFY(submit->isEnabled());
        filter->setText(QStringLiteral("destination"));
        int visible = 0;
        for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->isHidden()) ++visible;
        }
        QCOMPARE(visible, 1);
        filter->setText(QStringLiteral("not-there"));
        QVERIFY(!submit->isEnabled());
    }

    void lateProfileResolutionUpdatesOnlyTheDmRow()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        storage.addUser(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")},
        }, true);
        QVERIFY(addChannel(backend, QStringLiteral("source"), QStringLiteral("O"),
                           QStringLiteral("source")));
        QVERIFY(addChannel(backend, QStringLiteral("known"), QStringLiteral("O"),
                           QStringLiteral("known")));
        BackendChannel* dm = storage.addDirectChannel(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("dm")},
            {QStringLiteral("name"), QStringLiteral("self__unknown")},
            {QStringLiteral("display_name"), QStringLiteral("")},
            {QStringLiteral("type"), QStringLiteral("D")},
        });
        QVERIFY(dm);

        ForwardPostDialog dialog(backend, sourcePost(backend, QStringLiteral("source")));
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("forwardDestinations"));
        QVERIFY(list);
        QCOMPARE(list->count(), 3);
        QListWidgetItem* item = nullptr;
        for (int i = 0; i < list->count(); ++i) {
            auto* candidate = list->item(i);
            if (candidate->data(Qt::UserRole).toString() == QStringLiteral("dm")) {
                item = candidate;
                break;
            }
        }
        QVERIFY(item);
        QCOMPARE(item->text(), QStringLiteral("@unknown"));
        const int row = list->row(item);
        QListWidgetItem* selected = list->currentItem();
        QVERIFY(selected && selected != item);

        storage.addUser(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("unknown")},
            {QStringLiteral("username"), QStringLiteral("login")},
            {QStringLiteral("first_name"), QStringLiteral("Jane")},
            {QStringLiteral("last_name"), QStringLiteral("Doe")},
        });
        // Simulate a batched UserProfileService completion. No full row rebuild
        // or sorting is allowed even after a late name update.
        UserProfileService::instance(backend).profileResolved(QStringLiteral("unknown"));
        QCOMPARE(item->text(), QStringLiteral("@Jane Doe"));
        QCOMPARE(list->row(item), row);
        QCOMPARE(list->currentItem(), selected);
        QCOMPARE(item->data(Qt::UserRole).toString(), QStringLiteral("dm"));

        auto* search = dialog.findChild<QLineEdit*>(QStringLiteral("forwardDestinationFilter"));
        QVERIFY(search);
        search->setText(QStringLiteral("Jane"));
        QVERIFY(!item->isHidden());
        search->setText(QStringLiteral("absent"));
        QVERIFY(item->isHidden());
    }


    void forwardingUsesCanonicalUrlInRealPostPayload_data()
    {
        QTest::addColumn<bool>("usePermalink");
        QTest::newRow("native-permalink") << true;
        QTest::newRow("standalone-copy") << false;
    }

    void forwardingUsesCanonicalUrlInRealPostPayload()
    {
        QFETCH(bool, usePermalink);
        MLOptions::instance()->optionObject<bool>(
            FORWARD_WITH_PERMALINK, FORWARD_WITH_PERMALINK_DEFAULT)
            ->setValue(usePermalink);
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString connectionHost = QStringLiteral("http://127.0.0.1:%1/")
            .arg(server.serverPort());
        NetworkRequest::setHost(connectionHost);

        QJsonObject sentPost;
        int configRequests = 0;
        int postRequests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                QTcpSocket* socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, &server,
                        [&, socket, buffer, handled] {
                    if (*handled) return;
                    buffer->append(socket->readAll());
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const QByteArray headers = buffer->left(headerEnd);
                    int bodyLength = 0;
                    for (const QByteArray& rawLine : headers.split('\n')) {
                        const QByteArray line = rawLine.trimmed();
                        if (line.toLower().startsWith("content-length:")) {
                            bodyLength = line.mid(sizeof("content-length:") - 1)
                                .trimmed().toInt();
                        }
                    }
                    if (buffer->size() < headerEnd + 4 + bodyLength) return;
                    *handled = true;
                    const QByteArray requestLine = headers.left(headers.indexOf('\n')).trimmed();
                    QByteArray responseBody;
                    QByteArray status = "200 OK";
                    if (requestLine.startsWith("GET /api/v4/config/client?format=old ")) {
                        ++configRequests;
                        responseBody = R"({"SiteURL":"https://canonical.example.net","EnablePermalinkPreviews":"true"})";
                    } else if (requestLine.startsWith("POST /api/v4/posts ")) {
                        ++postRequests;
                        const QByteArray body = buffer->mid(headerEnd + 4, bodyLength);
                        sentPost = QJsonDocument::fromJson(body).object();
                        // We test the actual wire payload, not server-side
                        // persistence. A deliberate rejection keeps the dialog open.
                        status = "400 Bad Request";
                        responseBody = R"({"message":"mock transport received post"})";
                    } else {
                        status = "404 Not Found";
                        responseBody = "{}";
                    }
                    const QByteArray response = "HTTP/1.1 " + status
                        + "\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(responseBody.size())
                        + "\r\nConnection: close\r\n\r\n" + responseBody;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
            }
        });

        Backend backend;
        QVERIFY(addChannel(backend, QStringLiteral("source"),
                           QStringLiteral("O"), QStringLiteral("source")));
        QVERIFY(addChannel(backend, QStringLiteral("private"),
                           QStringLiteral("P"), QStringLiteral("private")));
        const QString sourceId = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
        BackendPost original(QJsonObject{
            {QStringLiteral("id"), sourceId},
            {QStringLiteral("channel_id"), QStringLiteral("source")},
            {QStringLiteral("message"), QStringLiteral("Original content")},
            {QStringLiteral("user_id"), QStringLiteral("original-author")},
        }, backend.getStorage());
        ForwardPostDialog dialog(backend, original);
        auto* destinations = dialog.findChild<QListWidget*>(
            QStringLiteral("forwardDestinations"));
        auto* submit = dialog.findChild<QPushButton*>(
            QStringLiteral("forwardSubmit"));
        auto* comment = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("forwardComment"));
        auto* error = dialog.findChild<QLabel*>(
            QStringLiteral("forwardError"));
        QVERIFY(destinations && submit && comment && error);
        for (int index = 0; index < destinations->count(); ++index) {
            if (destinations->item(index)->data(Qt::UserRole).toString()
                == QStringLiteral("private")) {
                destinations->setCurrentRow(index);
                break;
            }
        }
        QCOMPARE(destinations->currentItem()->data(Qt::UserRole).toString(),
                 QStringLiteral("private"));
        comment->setPlainText(QStringLiteral("Context"));
        QVERIFY(submit->isEnabled());
        dialog.show();
        submit->click();
        QTRY_COMPARE(postRequests, 1);
        QCOMPARE(configRequests, usePermalink ? 1 : 0);
        QCOMPARE(sentPost.value(QStringLiteral("channel_id")).toString(),
                 QStringLiteral("private"));
        if (usePermalink) {
            QCOMPARE(sentPost.value(QStringLiteral("message")).toString(),
                     QStringLiteral("Context\nhttps://canonical.example.net/team/pl/")
                         + sourceId);
            QVERIFY(sentPost.value(QStringLiteral("props")).toObject().isEmpty());
        } else {
            QCOMPARE(sentPost.value(QStringLiteral("message")).toString(),
                     QStringLiteral("Context\n\nOriginal content"));
            const QJsonObject props = sentPost.value(QStringLiteral("props")).toObject();
            QVERIFY(props.value(QStringLiteral("isForwarded")).toBool());
            QCOMPARE(props.value(QStringLiteral("originalPostId")).toString(), sourceId);
            QCOMPARE(props.value(QStringLiteral("originalAuthorId")).toString(),
                     QStringLiteral("original-author"));
            QCOMPARE(props.value(QStringLiteral("current_team_id")).toString(),
                     QStringLiteral("team"));
        }
        QVERIFY(!sentPost.value(QStringLiteral("message")).toString()
                    .contains(QStringLiteral("/_redirect/")));
        QTRY_VERIFY(error->isVisible());
        NetworkRequest::setHost(QStringLiteral("https://mattermost.example.test/"));
    }

    void incomingForwardIndicatorIsIndependentOfOutgoingSetting_data()
    {
        QTest::addColumn<bool>("usePermalink");
        QTest::newRow("outgoing-permalink-on") << true;
        QTest::newRow("outgoing-permalink-off") << false;
    }

    void incomingForwardIndicatorIsIndependentOfOutgoingSetting()
    {
        QFETCH(bool, usePermalink);
        MLOptions::instance()->optionObject<bool>(
            FORWARD_WITH_PERMALINK, FORWARD_WITH_PERMALINK_DEFAULT)
            ->setValue(usePermalink);
        Backend backend;
        QVERIFY(addChannel(backend, QStringLiteral("destination"),
                           QStringLiteral("P"), QStringLiteral("destination")));
        backend.getStorage().addUser(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("original-author")},
            {QStringLiteral("username"), QStringLiteral("original-author")},
            {QStringLiteral("first_name"), QStringLiteral("Original")},
            {QStringLiteral("last_name"), QStringLiteral("Author")},
        });
        BackendPost forward(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("abcdefghijklmnopqrstuvwxyz")},
            {QStringLiteral("channel_id"), QStringLiteral("destination")},
            {QStringLiteral("user_id"), QStringLiteral("forwarder")},
            {QStringLiteral("message"), QStringLiteral("Original **Markdown**")},
            {QStringLiteral("props"), QJsonObject{
                {QStringLiteral("isForwarded"), true},
                {QStringLiteral("originalPostId"), QStringLiteral("source-post")},
                {QStringLiteral("originalAuthorId"), QStringLiteral("original-author")},
            }},
        }, backend.getStorage());
        PostWidget widget(backend, forward, nullptr, nullptr, nullptr,
                          PostWidget::PresentationMode::ReadOnlySnapshot);
        auto* forwardedLabel = widget.findChild<QLabel*>(
            QStringLiteral("forwardedOriginLabel"));
        QVERIFY(forwardedLabel);
        QCOMPARE(forwardedLabel->text(),
                 QStringLiteral("Forwarded from Original Author"));
        QVERIFY(!forwardedLabel->isHidden());
        QCOMPARE(forward.message, QStringLiteral("Original **Markdown**"));
        QVERIFY(widget.findChildren<QuotedPostPreview*>().isEmpty());

        // Edits may replace metadata in place; don't leave a stale label.
        forward.props = QJsonObject{};
        widget.setEdited(forward.message);
        QVERIFY(forwardedLabel->isHidden());

        const QString originalId =
            QStringLiteral("abcdefghijklmnopqrstuvwx12");
        const QString url =
            QStringLiteral("https://canonical.example.net/team/pl/") + originalId;
        BackendPost permalinkPost(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("secondaryabcdefghijkl")},
            {QStringLiteral("channel_id"), QStringLiteral("destination")},
            {QStringLiteral("user_id"), QStringLiteral("forwarder")},
            {QStringLiteral("message"), url},
            {QStringLiteral("metadata"), QJsonObject{
                {QStringLiteral("embeds"), QJsonArray{QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("permalink")},
                    {QStringLiteral("data"), QJsonObject{
                        {QStringLiteral("post_id"), originalId},
                        {QStringLiteral("post"), QJsonObject{
                            {QStringLiteral("id"), originalId},
                            {QStringLiteral("user_id"), QStringLiteral("original-author")},
                            {QStringLiteral("message"), QStringLiteral("Source text")},
                        }},
                    }},
                }}},
            }},
        }, backend.getStorage());
        PostWidget permalinkWidget(backend, permalinkPost,
                                  nullptr, nullptr, nullptr,
                                  PostWidget::PresentationMode::ReadOnlySnapshot);
        QVERIFY(!permalinkWidget.findChild<QLabel*>(
            QStringLiteral("forwardedOriginLabel")));
        QCOMPARE(permalinkWidget.findChildren<QuotedPostPreview*>().size(), 1);
    }

    void standaloneModeDoesNotDropAttachments()
    {
        MLOptions::instance()->optionObject<bool>(
            FORWARD_WITH_PERMALINK, FORWARD_WITH_PERMALINK_DEFAULT)
            ->setValue(false);
        Backend backend;
        QVERIFY(addChannel(backend, QStringLiteral("source"),
                           QStringLiteral("O"), QStringLiteral("source")));
        QVERIFY(addChannel(backend, QStringLiteral("target"),
                           QStringLiteral("P"), QStringLiteral("target")));
        const BackendPost original(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("abcdefghijklmnopqrstuvwxyz")},
            {QStringLiteral("channel_id"), QStringLiteral("source")},
            {QStringLiteral("user_id"), QStringLiteral("original-author")},
            {QStringLiteral("message"), QStringLiteral("Attachment attached")},
            {QStringLiteral("metadata"), QJsonObject{
                {QStringLiteral("files"), QJsonArray{QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("existing-file")},
                    {QStringLiteral("name"), QStringLiteral("photo.png")},
                }}},
            }},
        }, backend.getStorage());
        ForwardPostDialog dialog(backend, original);
        auto* submit = dialog.findChild<QPushButton*>(
            QStringLiteral("forwardSubmit"));
        auto* error = dialog.findChild<QLabel*>(
            QStringLiteral("forwardError"));
        QVERIFY(submit && error);
        dialog.show();
        submit->click();
        QVERIFY(error->isVisible());
        QVERIFY(error->text().contains(QStringLiteral("attachments")));
    }

    void privateOriginOnlyOffersItsOwnChannel()
    {
        Backend backend;
        QVERIFY(addChannel(backend, QStringLiteral("private"), QStringLiteral("P"),
                           QStringLiteral("private")));
        QVERIFY(addChannel(backend, QStringLiteral("other"), QStringLiteral("O"),
                           QStringLiteral("other")));
        ForwardPostDialog dialog(backend, sourcePost(backend, QStringLiteral("private")));
        QCOMPARE(dialog.windowModality(), Qt::WindowModal);
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("forwardDestinations"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QStringLiteral("private"));
        QVERIFY(!dialog.findChild<QLineEdit*>(QStringLiteral("forwardDestinationFilter")));
        QVERIFY(dialog.findChild<QWidget*>(QStringLiteral("forwardPrivacyNotice")));
        QVERIFY(dialog.findChild<QPlainTextEdit*>(QStringLiteral("forwardComment")));
    }
};

QTEST_MAIN(ForwardPostDialogTest)
#include "ForwardPostDialogTest.moc"
