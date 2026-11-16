#include <QtTest>

#include <QJsonObject>
#include <QListWidget>
#include <QToolButton>

#include "build-config.h"
#include "backend/Backend.h"
#include "backend/types/BackendFile.h"
#include "chat-area/post/attachments/PostAttachmentList.h"
#if BUILD_MULTIMEDIA
#include "chat-area/post/attachments/AttachedVideoFile.h"
#include <QMediaPlayer>
#include <QPointer>
#endif

using namespace Mattermost;

class PostAttachmentListCompactTest : public QObject
{
    Q_OBJECT

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
        QVERIFY(card);

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
        QVERIFY(popout);
        popout->click();
        QVERIFY(player);
        QWidget* floating = player->window();
        QVERIFY(floating);
        QVERIFY(floating != attachments->window());
        QVERIFY(floating->isWindow());

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
