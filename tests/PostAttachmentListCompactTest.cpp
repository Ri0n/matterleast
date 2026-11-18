#include <QtTest>

#include <QJsonObject>
#include <QDesktopServices>
#include <QUrl>
#include <QPushButton>
#include <QFile>
#include <QFileInfo>
#include <QLayout>
#include <QTemporaryDir>
#include <QListWidget>
#include <QToolButton>

#include "build-config.h"
#include "backend/Backend.h"
#include "backend/AttachmentService.h"
#include "backend/types/BackendFile.h"
#include "chat-area/post/attachments/PostAttachmentList.h"
#include "chat-area/post/attachments/AttachmentSaveFile.h"
#include "chat-area/post/attachments/AttachedBinaryFile.h"
#include "chat-area/post/attachments/CompactAttachmentRow.h"
#if BUILD_MULTIMEDIA
#include "chat-area/post/attachments/AttachedVideoFile.h"
#include <QMediaPlayer>
#include <QPointer>
#endif

using namespace Mattermost;

class PostAttachmentListCompactTest : public QObject
{
    Q_OBJECT

    QUrl openedUrl;

private:
    static std::list<BackendFile> files(int count)
    {
        std::list<BackendFile> result;
        for (int i = 0; i < count; ++i) {
            QJsonObject json;
            json.insert(QStringLiteral("id"), QStringLiteral("id-%1").arg(i));
            json.insert(QStringLiteral("name"), QStringLiteral("file-%1.png").arg(i));
            json.insert(QStringLiteral("mime_type"), QStringLiteral("image/png"));
            json.insert(QStringLiteral("size"), 1024 * (i + 1));
            result.emplace_back(json);
        }
        return result;
    }

private slots:
    void captureLocalOpen(const QUrl& url)
    {
        openedUrl = url;
    }

    void savedAttachmentsOpenWithoutAnotherDownload()
    {
        Backend backend;
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const QByteArray payload("batch saved attachment");
        const QString saved = folder.filePath(QStringLiteral("custom-name.txt"));
        QFile output(saved);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(payload), payload.size());
        output.close();

        const QString fileId = QStringLiteral("batch-file-id");
        QJsonObject json {
            {QStringLiteral("id"), fileId},
            {QStringLiteral("name"), QStringLiteral("original-name.txt")},
            {QStringLiteral("mime_type"), QStringLiteral("text/plain")},
            {QStringLiteral("size"), payload.size()}
        };
        const BackendFile file(json);
        auto& service = AttachmentService::instance(backend);

        // The row/card is created before the batch save finishes, then the
        // successful transfer registers a non-default destination.
        CompactAttachmentRow compact(backend, file);
        AttachedBinaryFile regular(backend, file);
        service.rememberLocalFile(fileId, saved);
        QCOMPARE(service.localFilePath(fileId, payload.size()), QFileInfo(saved).absoluteFilePath());
        QCOMPARE(service.localFilePath(fileId, payload.size() + 1), QString());

        QDesktopServices::setUrlHandler("file", this, "captureLocalOpen");
        const auto openButton = [](QWidget& widget) -> QAbstractButton* {
            for (auto* button : widget.findChildren<QAbstractButton*>()) {
                if (button->toolTip() == QStringLiteral("Open file"))
                    return button;
            }
            return nullptr;
        };
        auto* compactOpen = openButton(compact);
        auto* regularOpen = openButton(regular);
        QVERIFY(compactOpen);
        QVERIFY(regularOpen);
        compactOpen->click();
        QCOMPARE(openedUrl, QUrl::fromLocalFile(QFileInfo(saved).absoluteFilePath()));
        openedUrl.clear();
        regularOpen->click();
        QCOMPARE(openedUrl, QUrl::fromLocalFile(QFileInfo(saved).absoluteFilePath()));
        QDesktopServices::unsetUrlHandler("file");

        // Removing a saved file must not leave a stale path in the registry.
        QVERIFY(QFile::remove(saved));
        QCOMPARE(service.localFilePath(fileId, payload.size()), QString());
    }

    void savedFileLookupFallsBackToOlderValidCopy()
    {
        Backend backend;
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        auto& service = AttachmentService::instance(backend);
        const QString id = QStringLiteral("same-attachment");
        const QByteArray data("same payload");
        const QString first = folder.filePath(QStringLiteral("first.txt"));
        const QString second = folder.filePath(QStringLiteral("second.txt"));
        for (const QString& path : {first, second}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size());
            file.close();
            service.rememberLocalFile(id, path);
        }
        QCOMPARE(service.localFilePath(id, data.size()), second);
        QVERIFY(QFile::remove(second));
        QCOMPARE(service.localFilePath(id, data.size()), first);
        QCOMPARE(service.localFilePath(QStringLiteral("different-id")), QString());
    }

    void fourAttachmentsUseCollapsedRowsWithoutPreviews()
    {
        Backend backend;
        PostAttachmentList attachments(backend);
        attachments.setFiles(files(5), QStringLiteral("Test"));

        auto* list = attachments.findChild<QListWidget*>();
        auto* link = attachments.findChild<QToolButton*>(
            QStringLiteral("attachmentShowMore"));
        QVERIFY(list);
        QVERIFY(link);
        QCOMPARE(list->count(), 5);
        QCOMPARE(attachments.findChildren<QWidget*>(QStringLiteral("compactAttachmentRow")).size(), 5);
        QVERIFY(!list->item(0)->isHidden());
        QVERIFY(!list->item(2)->isHidden());
        QVERIFY(list->item(3)->isHidden());
        QVERIFY(list->item(4)->isHidden());
        QVERIFY(link->text().contains(QStringLiteral("2")));

        const int foldedHeight = list->sizeHint().height();
        link->click();
        QVERIFY(!list->item(3)->isHidden());
        QVERIFY(!list->item(4)->isHidden());
        QVERIFY(list->sizeHint().height() > foldedHeight);
        QCOMPARE(link->text(), attachments.tr("Show less"));

        link->click();
        QVERIFY(list->item(3)->isHidden());
        QCOMPARE(list->sizeHint().height(), foldedHeight);
    }

#if BUILD_MULTIMEDIA
    void savingAlreadyCachedMediaAvoidsNetwork()
    {
        Backend backend;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cached = directory.filePath(QStringLiteral("cached.mp4"));
        const QString destination = directory.filePath(QStringLiteral("saved.mp4"));
        const QByteArray payload("local media bytes from cache");
        QFile source(cached);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(payload), payload.size());
        source.close();

        // The nonexistent remote ID must never be requested. Both the
        // thumbnail and the full original belong to the application cache.
        AttachmentSaveFile::saveToPath(
            backend, QStringLiteral("not-on-the-server"), destination,
            nullptr, cached);
        QTRY_VERIFY(QFileInfo::exists(destination));
        QFile saved(destination);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), payload);
    }

    void inlineVideoResizesAttachmentListAndDestroysWithoutCallbacks()
    {
        Backend backend;
        auto* attachments = new PostAttachmentList(backend);
        QJsonObject video {
            {QStringLiteral("id"), QStringLiteral("inline-video")},
            {QStringLiteral("name"), QStringLiteral("inline.mp4")},
            {QStringLiteral("mime_type"), QStringLiteral("video/mp4")},
            {QStringLiteral("size"), 4096},
        };
        attachments->setFiles({BackendFile(video)}, QStringLiteral("Test"));
        auto* list = attachments->findChild<QListWidget*>();
        auto* card = attachments->findChild<AttachedVideoFile*>();
        QVERIFY(list && card);
        const int originalRowHeight = list->item(0)->sizeHint().height();
        const int originalListHeight = list->height();
        const int originalPostHeight = attachments->sizeHint().height();
        QSignalSpy resized(attachments, &PostAttachmentList::dimensionsChanged);

        auto* launch = card->findChild<QToolButton*>(
            QStringLiteral("attachmentMediaLaunch"));
        QVERIFY(launch);
        launch->click();
        QPointer<QWidget> player = card->findChild<QWidget*>(
            QStringLiteral("qite-video-player"));
        QVERIFY(player);
        QVERIFY(list->item(0)->sizeHint().height() > originalRowHeight);
        QVERIFY(list->height() > originalListHeight);
        QVERIFY(attachments->sizeHint().height() > originalPostHeight);
        QVERIFY(resized.count() > 0);

        // An inline player dies with its post. No destroyed() callback can
        // touch the already-destroyed button from QWidget's child teardown.
        delete attachments;
        QVERIFY(player.isNull());
    }

    void inlineAudioOnlyReservesArtworkSpaceWhenCoverExists()
    {
        Backend backend;
        PostAttachmentList attachments(backend);
        QJsonObject audio {
            {QStringLiteral("id"), QStringLiteral("inline-audio")},
            {QStringLiteral("name"), QStringLiteral("voice.mp3")},
            {QStringLiteral("mime_type"), QStringLiteral("audio/mpeg")},
            {QStringLiteral("size"), 4096},
        };
        attachments.setFiles({BackendFile(audio)}, QStringLiteral("Test"));
        auto* card = attachments.findChild<AttachedVideoFile*>();
        auto* list = attachments.findChild<QListWidget*>();
        QVERIFY(card && list);
        auto* launch = card->findChild<QToolButton*>(
            QStringLiteral("attachmentMediaLaunch"));
        QVERIFY(launch);
        launch->click();
        auto* player = card->findChild<QWidget*>(
            QStringLiteral("qite-video-player"));
        QVERIFY(player);
        auto* art = player->findChild<QWidget*>(
            QStringLiteral("qite-audio-artwork"));
        auto* controls = player->findChild<QWidget*>(
            QStringLiteral("qite-video-controls"));
        QVERIFY(art && controls);
        QVERIFY(art->isHidden());
        QCOMPARE(player->height(), controls->sizeHint().height());
        const int compactHeight = list->item(0)->sizeHint().height();
        QSignalSpy changed(&attachments, &PostAttachmentList::dimensionsChanged);

        // Simulate Qite's asynchronous metadata notification. The attachment
        // must resize synchronously without pruning/rebuilding its rows.
        player->setProperty("qite-audio-artwork-visible", true);
        QTRY_VERIFY(list->item(0)->sizeHint().height() > compactHeight);
        QCOMPARE(player->height(), controls->sizeHint().height() + 120);
        player->setProperty("qite-audio-artwork-visible", false);
        QTRY_COMPARE(list->item(0)->sizeHint().height(), compactHeight);
        QCOMPARE(player->height(), controls->sizeHint().height());
        QVERIFY(changed.count() >= 2);
    }

    void detachedMediaPlayerSurvivesOriginalAttachmentDestruction()
    {
        Backend backend;
        auto* attachments = new PostAttachmentList(backend);
        QJsonObject video {
            {QStringLiteral("id"), QStringLiteral("test-video")},
            {QStringLiteral("name"), QStringLiteral("test.mp4")},
            {QStringLiteral("mime_type"), QStringLiteral("video/mp4")},
            {QStringLiteral("size"), 4096},
        };
        attachments->setFiles({BackendFile(video)}, QStringLiteral("Test"));
        auto* card = attachments->findChild<AttachedVideoFile*>();
        auto* list = attachments->findChild<QListWidget*>();
        QVERIFY(card);
        QVERIFY(list);

        auto* launch = card->findChild<QToolButton*>(
            QStringLiteral("attachmentMediaLaunch"));
        QVERIFY(launch);
        launch->click();

        QPointer<QWidget> player = card->findChild<QWidget*>(
            QStringLiteral("qite-video-player"));
        QVERIFY(player);
        QVERIFY(!player->testAttribute(Qt::WA_NativeWindow));
        QVERIFY(player->findChild<QWidget*>(
            QStringLiteral("qite-video-frame")));

        auto* popout = player->findChild<QToolButton*>(
            QStringLiteral("qite-video-window-button"));
        auto* fullscreen = player->findChild<QToolButton*>(
            QStringLiteral("qite-video-fullscreen-button"));
        QVERIFY(popout);
        QVERIFY(fullscreen);
        QVERIFY(!popout->isHidden()); // createPlayer() without host handler
        QVERIFY(!fullscreen->isHidden());
        popout->click();
        QVERIFY(player);
        QWidget* floating = player->window();
        QVERIFY(floating);
        QVERIFY(floating != attachments->window());
        QVERIFY(floating->isWindow());
        QCoreApplication::processEvents();
        const int floatingRowHeight = list->item(0)->sizeHint().height();

        // Returning to an existing message must put the *same* player back
        // under its QVBoxLayout. Merely changing QWidget::parent is insufficient.
        popout->click();
        QTRY_COMPARE(player->parentWidget(), static_cast<QWidget*>(card));
        QVERIFY(card->layout()->indexOf(player) >= 0);
        QVERIFY(list->item(0)->sizeHint().height() > floatingRowHeight);
        popout->click();
        QTRY_VERIFY(player->window() != attachments->window());
        floating = player->window();

        // The QListWidget/ChatArea may discard the original post. The popup
        // must now be the owner and keep the same player alive.
        delete attachments;
        QVERIFY(player);
        QCOMPARE(player->window(), floating);
        floating->close();
        QTRY_VERIFY(player.isNull());
    }

    void videoAndAudioPreviewsStayLazyAndNonNative()
    {
        Backend backend;
        PostAttachmentList attachments(backend);
        std::list<BackendFile> media;
        for (const auto& spec : {
                 QPair<QString, QString>{QStringLiteral("sample.mp4"), QStringLiteral("video/mp4")},
                 QPair<QString, QString>{QStringLiteral("voice.ogg"), QStringLiteral("audio/ogg")}}) {
            QJsonObject json {
                {QStringLiteral("id"), spec.first},
                {QStringLiteral("name"), spec.first},
                {QStringLiteral("mime_type"), spec.second},
                {QStringLiteral("size"), 4096},
            };
            media.emplace_back(json);
        }
        attachments.setFiles(media, QStringLiteral("Test"));

        const auto cards = attachments.findChildren<AttachedVideoFile*>();
        QCOMPARE(cards.size(), 2);
        for (auto* card : cards) {
            QVERIFY(!card->isWindow());
            QVERIFY(!card->testAttribute(Qt::WA_NativeWindow));
            QVERIFY(card->findChildren<QMediaPlayer*>().isEmpty());
            QVERIFY(!card->findChild<QWidget*>(
                QStringLiteral("qite-video-frame")));
            QVERIFY(card->findChild<QToolButton*>(
                QStringLiteral("attachmentMediaLaunch")));
        }
    }
#endif

    void threeFilesKeepExistingCards()
    {
        Backend backend;
        PostAttachmentList attachments(backend);
        // Non-image files avoid attempting network previews in this test.
        auto data = files(3);
        std::list<BackendFile> binaries;
        for (const auto& value : data) {
            QJsonObject json {
                {QStringLiteral("id"), value.id},
                {QStringLiteral("name"), value.name + QStringLiteral(".txt")},
                {QStringLiteral("mime_type"), QStringLiteral("text/plain")},
                {QStringLiteral("size"), 1024},
            };
            binaries.emplace_back(json);
        }
        attachments.setFiles(binaries, QStringLiteral("Test"));

        auto* list = attachments.findChild<QListWidget*>();
        QVERIFY(list);
        QCOMPARE(list->count(), 3);
        QVERIFY(!attachments.findChild<QToolButton*>(
            QStringLiteral("attachmentShowMore")));
        QCOMPARE(attachments.findChildren<QWidget*>(QStringLiteral("compactAttachmentRow")).size(), 0);
    }
};

QTEST_MAIN(PostAttachmentListCompactTest)
#include "PostAttachmentListCompactTest.moc"
