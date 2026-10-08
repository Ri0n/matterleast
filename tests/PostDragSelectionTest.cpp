#include <QtTest>

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextLayout>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendTeam.h"
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
const QString ChannelSlug = QStringLiteral("linked-channel");

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
    QString secondMessage;
    bool secondHasImage = true;

    Server()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
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
                    const QJsonObject image =
                        post(ImagePostId, secondMessage, base + 1000, secondHasImage);

                    QByteArray contentType = "application/json";
                    QByteArray body = "[]";
                    if (path.startsWith(QStringLiteral("/api/v4/files/") + FileId)) {
                        contentType = "image/png";
                        body = portraitPng();
                    } else if (path == QStringLiteral("/api/v4/teams/name/team/channels/name/")
                                   + ChannelSlug) {
                        body = QJsonDocument(QJsonObject {
                            {QStringLiteral("id"), QStringLiteral("linkedlinkedlinkedlinkedli")},
                            {QStringLiteral("name"), ChannelSlug},
                            {QStringLiteral("display_name"), QStringLiteral("Linked Channel")},
                        }).toJson(QJsonDocument::Compact);
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

struct Thread {
    std::unique_ptr<Server> server;
    std::unique_ptr<Backend> backend;
    std::unique_ptr<ChatArea> area;
    ChatLogWidget* log = nullptr;
    PostWidget* rootPost = nullptr;
    PostWidget* secondPost = nullptr;
};

void openThread(Thread& thread, const QString& secondMessage, bool secondHasImage)
{
    thread.server = std::make_unique<Server>();
    thread.server->secondMessage = secondMessage;
    thread.server->secondHasImage = secondHasImage;
    QVERIFY(thread.server->listen(QHostAddress::LocalHost));
    NetworkRequest::setHost(
        QStringLiteral("http://127.0.0.1:%1/").arg(thread.server->serverPort()));
    thread.backend = std::make_unique<Backend>();
    Storage& storage = thread.backend->getStorage();
    storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
    BackendTeam* team = storage.addTeam(
        QJsonObject {{"id", "team"}, {"name", "team"}, {"display_name", "Team"}});
    QVERIFY(team);
    BackendChannel* channel = storage.addTeamChannel(*team, QJsonObject {
        {"id", "channel"}, {"type", "O"}, {"name", "chan"},
        {"display_name", "Chan"}, {"team_id", "team"}});
    QVERIFY(channel);

    thread.area = std::make_unique<ChatArea>(*thread.backend, *channel, RootId, nullptr);
    thread.area->resize(900, 700);
    thread.area->show();
    QVERIFY(QTest::qWaitForWindowExposed(thread.area.get()));
    thread.area->goToNewest();

    thread.log = thread.area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
    QVERIFY(thread.log);
    QTRY_VERIFY(thread.log->findPost(RootId) && thread.log->findPost(ImagePostId));
    thread.rootPost = thread.log->findPost(RootId);
    thread.secondPost = thread.log->findPost(ImagePostId);
    QTRY_VERIFY(thread.rootPost->isVisible() && thread.secondPost->isVisible());
}

AttachedImageFile* loadedAttachment(PostWidget* post)
{
    auto* attachment = post->findChild<AttachedImageFile*>();
    if (!attachment) {
        return nullptr;
    }
    auto* preview = attachment->findChild<QLabel*>(QStringLiteral("imagePreview"));
    const bool loaded = QTest::qWaitFor(
        [preview] { return hasPixmap(preview) && preview->isVisible(); }, 5000);
    return loaded ? attachment : nullptr;
}

QTextBrowser* resolvedChannelLink(PostWidget* post)
{
    QTextBrowser* result = nullptr;
    (void)QTest::qWaitFor([&] {
        for (QTextBrowser* browser : post->findChildren<QTextBrowser*>()) {
            if (browser->isVisible()
                && browser->toPlainText().contains(QStringLiteral("Linked Channel"))) {
                result = browser;
                return true;
            }
        }
        return false;
    }, 5000);
    return result;
}

QRectF firstLineRect(QTextBrowser* browser)
{
    const QTextBlock block = browser->document()->firstBlock();
    const QTextLine line = block.layout()->lineAt(0);
    return line.naturalTextRect().translated(block.layout()->position());
}

} // namespace

class PostDragSelectionTest : public QObject
{
    Q_OBJECT

private slots:
    void cleanup()
    {
        for (QWidget* topLevel : QApplication::topLevelWidgets()) {
            if (qobject_cast<FilePreview*>(topLevel)) {
                topLevel->close();
            }
        }
    }

    void dragOutOfPostSelectsMessages_data()
    {
        QTest::addColumn<QString>("start");
        QTest::newRow("beside image") << QStringLiteral("besideImage");
        QTest::newRow("on image") << QStringLiteral("onImage");
        QTest::newRow("beside channel link") << QStringLiteral("besideChannelLink");
        QTest::newRow("on channel link") << QStringLiteral("onChannelLink");
    }

    void dragOutOfPostSelectsMessages()
    {
        QFETCH(QString, start);
        const bool channelPost = start.contains(QStringLiteral("Channel"));

        Thread thread;
        openThread(thread,
                   channelPost ? QStringLiteral("~") + ChannelSlug : QString(),
                   !channelPost);
        if (QTest::currentTestFailed()) {
            return;
        }
        ChatArea& area = *thread.area;

        QPoint pressPoint;
        if (channelPost) {
            QTextBrowser* browser = resolvedChannelLink(thread.secondPost);
            QVERIFY(browser);
            const QRectF line = firstLineRect(browser);
            const QPoint local = start == QStringLiteral("onChannelLink")
                ? line.center().toPoint()
                : QPoint(qRound(line.right()) + 60, qRound(line.center().y()));
            QVERIFY(browser->viewport()->rect().contains(local));
            pressPoint = browser->viewport()->mapTo(&area, local);
        } else {
            AttachedImageFile* attachment = loadedAttachment(thread.secondPost);
            QVERIFY(attachment);
            pressPoint = start == QStringLiteral("onImage")
                ? attachment->mapTo(&area, attachment->rect().center())
                : attachment->mapTo(&area, QPoint(attachment->width() + 80,
                                                  attachment->height() / 2));
        }
        QTest::qWait(50);

        const QRect secondRect(thread.secondPost->mapTo(&area, QPoint()),
                               thread.secondPost->size());
        QVERIFY(secondRect.contains(pressPoint));
        const QPoint abovePost =
            thread.rootPost->mapTo(&area, thread.rootPost->rect().center());

        QWindow* window = area.windowHandle();
        QTest::mousePress(window, Qt::LeftButton, {}, pressPoint);
        // Moving inside the first post keeps ordinary text interaction.
        const QPoint insideFirst(pressPoint.x(), secondRect.top() + 2);
        QTest::mouseMove(window, pressPoint + QPoint(0, -1));
        QTest::mouseMove(window, insideFirst);
        QVERIFY(!thread.log->isMessageSelectionMode());

        // Leaving the first post starts message selection.
        QTest::mouseMove(window, QPoint(insideFirst.x(), secondRect.top() - 2));
        QVERIFY(thread.log->isMessageSelectionMode());
        QVERIFY(thread.secondPost->wholeMessageSelected());
        QVERIFY(thread.secondPost->getSelectedText().isEmpty());

        QTest::mouseMove(window, abovePost);
        QVERIFY(thread.secondPost->wholeMessageSelected());
        QVERIFY(thread.rootPost->wholeMessageSelected());

        QTest::mouseRelease(window, Qt::LeftButton, {}, abovePost);
        QVERIFY(thread.secondPost->wholeMessageSelected());
        QVERIFY(thread.rootPost->wholeMessageSelected());
        QTest::qWait(200);
        QVERIFY(!previewOpen());
    }

    void laterPostsJoinSelectionOnEnter()
    {
        Thread thread;
        openThread(thread, QStringLiteral("~") + ChannelSlug, false);
        if (QTest::currentTestFailed()) {
            return;
        }
        ChatArea& area = *thread.area;
        QTextBrowser* rootBrowser = nullptr;
        QTRY_VERIFY((rootBrowser = thread.rootPost->findChild<QTextBrowser*>(
                         QStringLiteral("messageRichText"))));
        QTest::qWait(50);

        const QPoint onRootText =
            rootBrowser->viewport()->mapTo(&area, firstLineRect(rootBrowser).center().toPoint());
        const QRect rootRect(thread.rootPost->mapTo(&area, QPoint()), thread.rootPost->size());
        const QPoint secondTop(onRootText.x(),
                               thread.secondPost->mapTo(&area, QPoint()).y() + 2);

        QWindow* window = area.windowHandle();
        QTest::mousePress(window, Qt::LeftButton, {}, onRootText);
        QTest::mouseMove(window, onRootText + QPoint(30, 0));
        QVERIFY(!thread.log->isMessageSelectionMode());
        QVERIFY(!thread.rootPost->getSelectedText().isEmpty());

        QTest::mouseMove(window, QPoint(onRootText.x(), rootRect.bottom() + 1));
        QVERIFY(thread.log->isMessageSelectionMode());
        QVERIFY(thread.rootPost->getSelectedText().isEmpty());

        QTest::mouseMove(window, secondTop);
        QVERIFY(thread.rootPost->wholeMessageSelected());
        QVERIFY(thread.secondPost->wholeMessageSelected());

        // Coming back shrinks the range to the anchor post again.
        QTest::mouseMove(window, onRootText);
        QVERIFY(thread.rootPost->wholeMessageSelected());
        QVERIFY(!thread.secondPost->wholeMessageSelected());
        QTest::mouseRelease(window, Qt::LeftButton, {}, onRootText);
    }

    void clickOnImageStillOpensPreview()
    {
        Thread thread;
        openThread(thread, QString(), true);
        if (QTest::currentTestFailed()) {
            return;
        }
        AttachedImageFile* attachment = loadedAttachment(thread.secondPost);
        QVERIFY(attachment);
        QTest::qWait(50);

        QTest::mouseClick(thread.area->windowHandle(), Qt::LeftButton, {},
                          attachment->mapTo(thread.area.get(), attachment->rect().center()));
        QTRY_VERIFY(previewOpen());
        QVERIFY(!thread.log->isMessageSelectionMode());
    }
};

QTEST_MAIN(PostDragSelectionTest)
#include "PostDragSelectionTest.moc"
