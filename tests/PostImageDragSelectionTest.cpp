#include <QtTest>

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "chat-area/post/PostWidget.h"
#include "chat-area/post/attachments/AttachedImageFile.h"
#include "preview-window/FilePreview.h"

using namespace Mattermost;

namespace {

const QString RootId = QStringLiteral("rootrootrootrootrootrootro");
const QString ImagePostId = QStringLiteral("imageimageimageimageimagei");
const QString FileId = QStringLiteral("ffffffffffffffffffffffffff");

QByteArray portraitPng()
{
    QImage image(QSize(60, 120), QImage::Format_ARGB32);
    image.fill(Qt::darkGreen);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QJsonObject post(const QString& id, const QString& message, qint64 createAt,
                 bool withImage)
{
    QJsonObject result {
        {QStringLiteral("id"), id},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("user_id"), QStringLiteral("user")},
        {QStringLiteral("root_id"), id == RootId ? QString() : RootId},
        {QStringLiteral("message"), message},
        {QStringLiteral("create_at"), static_cast<double>(createAt)},
        {QStringLiteral("update_at"), static_cast<double>(createAt)},
    };
    if (id == RootId) {
        result.insert(QStringLiteral("reply_count"), 1);
        result.insert(QStringLiteral("last_reply_at"), static_cast<double>(createAt + 1000));
    }
    if (withImage) {
        result.insert(QStringLiteral("metadata"), QJsonObject {
            {QStringLiteral("files"), QJsonArray {QJsonObject {
                {QStringLiteral("id"), FileId},
                {QStringLiteral("name"), QStringLiteral("portrait.png")},
                {QStringLiteral("mime_type"), QStringLiteral("image/png")},
                {QStringLiteral("extension"), QStringLiteral("png")},
            }}},
        });
    }
    return result;
}

class Server final : public QTcpServer
{
public:
    Server()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    const QByteArray input =
                        socket->property("input").toByteArray() + socket->readAll();
                    socket->setProperty("input", input);
                    if (!input.contains("\r\n\r\n") || socket->property("replied").toBool()) {
                        return;
                    }
                    socket->setProperty("replied", true);
                    const QString path =
                        QUrl(QString::fromUtf8(input.split(' ').value(1))).path();

                    const qint64 base = 1790000000000LL;
                    const QJsonObject root =
                        post(RootId, QStringLiteral("Message above the picture"), base, false);
                    const QJsonObject image = post(ImagePostId, QString(), base + 1000, true);

                    QByteArray contentType = "application/json";
                    QByteArray body = "[]";
                    if (path.startsWith(QStringLiteral("/api/v4/files/") + FileId)) {
                        contentType = "image/png";
                        body = portraitPng();
                    } else if (path == QStringLiteral("/api/v4/posts/") + RootId) {
                        body = QJsonDocument(root).toJson(QJsonDocument::Compact);
                    } else if (path == QStringLiteral("/api/v4/posts/") + RootId
                                           + QStringLiteral("/thread")) {
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("posts"), QJsonObject {
                                {RootId, root}, {ImagePostId, image}}},
                            {QStringLiteral("order"), QJsonArray {RootId, ImagePostId}},
                            {QStringLiteral("has_next"), false},
                        }).toJson(QJsonDocument::Compact);
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + contentType
                                  + "\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

bool previewOpen()
{
    for (QWidget* topLevel : QApplication::topLevelWidgets()) {
        if (qobject_cast<FilePreview*>(topLevel) && topLevel->isVisible()) {
            return true;
        }
    }
    return false;
}

bool hasPixmap(const QLabel* label)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    return label && !label->pixmap(Qt::ReturnByValue).isNull();
#else
    return label && !label->pixmap().isNull();
#endif
}

} // namespace

class PostImageDragSelectionTest : public QObject
{
    Q_OBJECT

private slots:
    void dragSelectsMessages_data()
    {
        QTest::addColumn<bool>("fromImage");
        QTest::newRow("beside image") << false;
        QTest::newRow("on image") << true;
    }

    void dragSelectsMessages()
    {
        QFETCH(bool, fromImage);
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = backend.getStorage().addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"}});

        ChatArea area(backend, *channel, RootId, nullptr);
        area.resize(900, 700);
        area.show();
        QVERIFY(QTest::qWaitForWindowExposed(&area));
        area.goToNewest();

        auto* log = area.findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(log);
        QTRY_VERIFY(log->findPost(RootId) && log->findPost(ImagePostId));
        PostWidget* rootPost = log->findPost(RootId);
        PostWidget* imagePost = log->findPost(ImagePostId);

        auto* attachment = imagePost->findChild<AttachedImageFile*>();
        QVERIFY(attachment);
        auto* preview = attachment->findChild<QLabel*>(QStringLiteral("imagePreview"));
        QTRY_VERIFY(hasPixmap(preview) && preview->isVisible());
        QTRY_VERIFY(rootPost->isVisible() && imagePost->isVisible());
        QTest::qWait(50);

        // Empty space to the right of the portrait image is the attachment
        // QListWidget viewport, still inside the post.
        const QPoint besideImage = fromImage
            ? preview->mapTo(&area, preview->rect().center())
            : attachment->mapTo(
                  &area, QPoint(attachment->width() + 80, attachment->height() / 2));
        QVERIFY(imagePost->rect().contains(imagePost->mapFrom(&area, besideImage)));
        QCOMPARE(attachment->rect().contains(attachment->mapFrom(&area, besideImage)),
                 fromImage);
        const QPoint abovePost = rootPost->mapTo(&area, rootPost->rect().center());

        QWindow* window = area.windowHandle();
        QTest::mousePress(window, Qt::LeftButton, {}, besideImage);
        QTest::mouseMove(window, besideImage + QPoint(0, -10));
        QTest::mouseMove(window, abovePost);
        QTest::mouseMove(window, abovePost + QPoint(0, -2));

        QVERIFY(log->isMessageSelectionMode());
        QVERIFY(imagePost->wholeMessageSelected());
        QVERIFY(rootPost->wholeMessageSelected());

        QTest::mouseRelease(window, Qt::LeftButton, {}, abovePost + QPoint(0, -2));
        QVERIFY(imagePost->wholeMessageSelected());
        QVERIFY(rootPost->wholeMessageSelected());
        QTest::qWait(200);
        QVERIFY(!previewOpen());
    }

    void clickOnImageStillOpensPreview()
    {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = backend.getStorage().addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"}});

        ChatArea area(backend, *channel, RootId, nullptr);
        area.resize(900, 700);
        area.show();
        QVERIFY(QTest::qWaitForWindowExposed(&area));
        area.goToNewest();

        auto* log = area.findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QTRY_VERIFY(log->findPost(ImagePostId));
        auto* attachment = log->findPost(ImagePostId)->findChild<AttachedImageFile*>();
        QVERIFY(attachment);
        auto* preview = attachment->findChild<QLabel*>(QStringLiteral("imagePreview"));
        QTRY_VERIFY(hasPixmap(preview) && preview->isVisible());
        QTest::qWait(50);

        QTest::mouseClick(area.windowHandle(), Qt::LeftButton, {},
                          preview->mapTo(&area, preview->rect().center()));
        QTRY_VERIFY(previewOpen());
        QVERIFY(!log->isMessageSelectionMode());
        for (QWidget* topLevel : QApplication::topLevelWidgets()) {
            if (qobject_cast<FilePreview*>(topLevel)) {
                topLevel->close();
            }
        }
    }
};

QTEST_MAIN(PostImageDragSelectionTest)
#include "PostImageDragSelectionTest.moc"
