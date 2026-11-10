#include <QtTest>

#include <QJsonObject>
#include <QListWidget>
#include <QToolButton>

#include "backend/Backend.h"
#include "backend/types/BackendFile.h"
#include "chat-area/post/attachments/PostAttachmentList.h"

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
        QCOMPARE(attachments.findChildren<CompactAttachmentRow*>().size(), 0);
    }
};

QTEST_MAIN(PostAttachmentListCompactTest)
#include "PostAttachmentListCompactTest.moc"
