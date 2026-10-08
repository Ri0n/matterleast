#include <QtTest>

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendPost.h"
#include "chat-area/post/PostWidget.h"
#include "chat-area/post/attachments/AttachedImageFile.h"
#include "preview-window/FilePreview.h"

using namespace Mattermost;

namespace {

const QString FileId = QStringLiteral("ffffffffffffffffffffffffff");
const QSize OriginalSize(96, 72);
const QSize PreviewSize(48, 36);

QByteArray pngBytes(const QSize& size, const QColor& color)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

class FileServer final : public QTcpServer
{
public:
    QStringList requests;

    FileServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QByteArray input =
                        socket->property("input").toByteArray() + socket->readAll();
                    socket->setProperty("input", input);
                    if (!input.contains("\r\n\r\n")) {
                        return;
                    }
                    const QString path = QUrl(QString::fromUtf8(input.split(' ').value(1))).path();
                    requests.push_back(path);
                    const QString filePath = QStringLiteral("/api/v4/files/") + FileId;
                    QByteArray body;
                    if (path == filePath) {
                        body = pngBytes(OriginalSize, Qt::red);
                    } else if (path == filePath + QStringLiteral("/preview")
                               || path == filePath + QStringLiteral("/thumbnail")) {
                        body = pngBytes(PreviewSize, Qt::blue);
                    }
                    const QByteArray status = body.isEmpty() ? "404 Not Found" : "200 OK";
                    socket->write("HTTP/1.1 " + status
                                  + "\r\nContent-Type: image/png\r\nConnection: close\r\n"
                                    "Content-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

BackendPost makePost(Storage& storage, const QString& message)
{
    return BackendPost(
        QJsonObject {
            {QStringLiteral("id"), QStringLiteral("pppppppppppppppppppppppppp")},
            {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
            {QStringLiteral("user_id"), QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu")},
            {QStringLiteral("create_at"), 1790000000000.0},
            {QStringLiteral("message"), message},
            {QStringLiteral("metadata"), QJsonObject {
                {QStringLiteral("files"), QJsonArray {QJsonObject {
                    {QStringLiteral("id"), FileId},
                    {QStringLiteral("name"), QStringLiteral("mug.png")},
                    {QStringLiteral("mime_type"), QStringLiteral("image/png")},
                    {QStringLiteral("extension"), QStringLiteral("png")},
                }}},
            }},
        },
        storage);
}

// Right-clicks through the QWindow so Qt itself picks the receiving child
// widget and synthesizes QContextMenuEvent, exactly like a real mouse click.
QStringList rightClickMenu(QWidget& window, QWidget* target, const QPoint& targetPos,
                           const QString& actionToTrigger = {})
{
    QStringList actions;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            return;
        }
        poll.stop();
        QAction* trigger = nullptr;
        for (QAction* action : menu->actions()) {
            if (action->isSeparator()) {
                continue;
            }
            actions.push_back(action->text());
            if (action->text() == actionToTrigger) {
                trigger = action;
            }
        }
        if (trigger) {
            menu->setActiveAction(trigger);
            trigger->trigger();
        }
        menu->close();
    });
    poll.start();

    const QPoint windowPos = target->mapTo(&window, targetPos);
    QTest::mouseClick(window.windowHandle(), Qt::RightButton, {}, windowPos);
    QDeadlineTimer deadline(2000);
    while (poll.isActive() && !deadline.hasExpired()) {
        QTest::qWait(10);
    }
    return actions;
}

QRect inlineImageRect(QTextBrowser* browser)
{
    QTextDocument* document = browser->document();
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.charFormat().isImageFormat()) {
                continue;
            }
            const QTextLine line =
                block.layout()->lineForTextPosition(fragment.position() - block.position());
            const qreal x = line.cursorToX(fragment.position() - block.position());
            const QPointF origin = block.layout()->position();
            const QSize size = PreviewSize;
            return QRect(QPoint(qRound(origin.x() + x),
                                qRound(origin.y() + line.y() + line.ascent()) - size.height()),
                         size);
        }
    }
    return {};
}

} // namespace

class PostImageContextMenuTest : public QObject
{
    Q_OBJECT

private:
    FileServer _server;
    std::unique_ptr<Backend> _backend;

private slots:
    void initTestCase()
    {
        QVERIFY(_server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(_server.serverPort()));
        _backend = std::make_unique<Backend>();
    }

    void init()
    {
        QApplication::clipboard()->clear();
    }

    void attachedImageOffersCopyOfOriginal()
    {
        BackendPost post = makePost(_backend->getStorage(), QStringLiteral("Look at this mug"));
        PostWidget widget(*_backend, post, nullptr, nullptr, nullptr);
        widget.resize(640, 400);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        auto* attachment = widget.findChild<AttachedImageFile*>();
        QVERIFY(attachment);
        auto* preview = attachment->findChild<QLabel*>(QStringLiteral("imagePreview"));
        QVERIFY(preview);
        QTRY_VERIFY(!preview->pixmap().isNull() && preview->isVisible());
        QTRY_VERIFY(preview->mapTo(&widget, QPoint()).y() > 0);

        const QStringList actions = rightClickMenu(
            widget, preview, preview->rect().center(), QStringLiteral("Copy image"));
        QVERIFY2(actions.contains(QStringLiteral("Copy image")),
                 qPrintable(actions.join(QStringLiteral(", "))));
        QVERIFY(actions.contains(QStringLiteral("Copy post message")));

        QTRY_VERIFY(!QApplication::clipboard()->image().isNull());
        QCOMPARE(QApplication::clipboard()->image().size(), OriginalSize);
        QCOMPARE(QApplication::clipboard()->image().pixelColor(1, 1), QColor(Qt::red));

        // The right-button release must not also open the full-size preview.
        QTest::qWait(200);
        for (QWidget* topLevel : QApplication::topLevelWidgets()) {
            QVERIFY(!qobject_cast<FilePreview*>(topLevel));
        }
    }

    void messageTextDoesNotOfferCopyImage()
    {
        BackendPost post = makePost(_backend->getStorage(), QStringLiteral("Look at this mug"));
        PostWidget widget(*_backend, post, nullptr, nullptr, nullptr);
        widget.resize(640, 400);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        auto* attachment = widget.findChild<AttachedImageFile*>();
        QVERIFY(attachment);
        auto* preview = attachment->findChild<QLabel*>(QStringLiteral("imagePreview"));
        QTRY_VERIFY(preview && !preview->pixmap().isNull());

        auto* browser = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(browser);
        const QStringList actions =
            rightClickMenu(widget, browser->viewport(), QPoint(4, 4));
        QVERIFY(actions.contains(QStringLiteral("Copy post message")));
        QVERIFY2(!actions.contains(QStringLiteral("Copy image")),
                 qPrintable(actions.join(QStringLiteral(", "))));
    }

    void inlineMarkdownImageOffersCopyOfOriginal()
    {
        BackendPost post = makePost(
            _backend->getStorage(),
            QStringLiteral("Look at this mug\n\n![mug](/api/v4/files/%1) trailing text").arg(FileId));
        PostWidget widget(*_backend, post, nullptr, nullptr, nullptr);
        widget.resize(640, 400);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));

        QVERIFY(!widget.findChild<AttachedImageFile*>());
        QTextBrowser* browser = nullptr;
        QTRY_VERIFY([&] {
            for (QTextBrowser* candidate : widget.findChildren<QTextBrowser*>()) {
                QTextDocument* document = candidate->document();
                const QVariant resource = document->resource(
                    QTextDocument::ImageResource,
                    QUrl(QStringLiteral("/api/v4/files/") + FileId));
                if (!resource.value<QImage>().isNull()) {
                    browser = candidate;
                    return true;
                }
            }
            return false;
        }());
        QTRY_VERIFY(browser->height() > PreviewSize.height());

        const QRect imageRect = inlineImageRect(browser);
        QVERIFY(imageRect.isValid());

        const QStringList onImage = rightClickMenu(
            widget, browser->viewport(), imageRect.center(), QStringLiteral("Copy image"));
        QVERIFY2(onImage.contains(QStringLiteral("Copy image")),
                 qPrintable(onImage.join(QStringLiteral(", "))));
        QTRY_VERIFY(!QApplication::clipboard()->image().isNull());
        QCOMPARE(QApplication::clipboard()->image().size(), OriginalSize);

        const QStringList onText = rightClickMenu(
            widget, browser->viewport(),
            QPoint(imageRect.right() + 40, imageRect.center().y()));
        QVERIFY2(!onText.contains(QStringLiteral("Copy image")),
                 qPrintable(onText.join(QStringLiteral(", "))));
    }
};

QTEST_MAIN(PostImageContextMenuTest)
#include "PostImageContextMenuTest.moc"
