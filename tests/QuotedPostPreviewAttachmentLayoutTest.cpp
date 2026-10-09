#include <QtTest>

#include <QJsonObject>
#include <QLabel>
#include <QPushButton>

#include "backend/Backend.h"
#include "backend/types/BackendFile.h"
#include "chat-area/QuotedPostPreview.h"
#include "chat-area/post/attachments/AttachedBinaryFile.h"
#include "chat-area/post/attachments/PostAttachmentList.h"

using namespace Mattermost;

class QuotedPostPreviewAttachmentLayoutTest final : public QObject
{
    Q_OBJECT

private slots:
    void permalinkBinaryFileUsesPreviewWidth()
    {
        Backend backend;
        QuotedPostPreview preview(nullptr, 4);
        preview.setPreview(
            QStringLiteral("Sergey · Extended Platform Chat"),
            QStringLiteral("Could someone help with the server logs?"),
            true);
        const BackendFile file(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("file00000000000000000000001")},
            {QStringLiteral("name"), QStringLiteral("test_hw.tar")},
            {QStringLiteral("mime_type"), QStringLiteral("application/x-compressed-tar")},
            {QStringLiteral("size"), 230932},
        });
        preview.setInteractiveAttachments(
            backend, std::list<BackendFile> {file}, QStringLiteral("Sergey"));
        preview.resize(650, 300);
        preview.show();

        auto* list = preview.findChild<PostAttachmentList*>();
        auto* card = preview.findChild<AttachedBinaryFile*>();
        QVERIFY(list);
        QVERIFY(card);
        auto* metadata = card->findChild<QLabel*>(QStringLiteral("metadataLabel"));
        auto* download = card->findChild<QPushButton*>(
            QStringLiteral("downloadButton"));
        auto* open = card->findChild<QPushButton*>(QStringLiteral("openButton"));
        QVERIFY(metadata);
        QVERIFY(download);
        QVERIFY(open);

        QTRY_VERIFY_WITH_TIMEOUT(list->width() >= 500, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(metadata->width() >= 260, 2000);
        // Qt 5 and Qt 6 may assign different heights to QListWidget's
        // containing item widget. What matters for the original regression
        // is the width-dependent wrapping of the metadata itself, not the
        // outer card's total allocated height.
        const int metadataTextHeight = metadata->heightForWidth(metadata->width());
        QVERIFY2(metadataTextHeight > 0
                     && metadataTextHeight <= 6 * metadata->fontMetrics().lineSpacing(),
                 qPrintable(QStringLiteral("Metadata wraps too deeply: width=%1 textHeight=%2")
                                .arg(metadata->width()).arg(metadataTextHeight)));
        QVERIFY2(download->geometry().right() < open->geometry().left()
                     || download->mapToGlobal(download->rect().topRight()).x()
                            < open->mapToGlobal(open->rect().topLeft()).x(),
                 "Download and open buttons must not overlap");
        QVERIFY(list->rect().contains(list->mapFromGlobal(
            open->mapToGlobal(open->rect().center()))));

        // Resizing the containing chat must not return the file to its
        // initial narrow intrinsic list size.
        preview.resize(460, 300);
        QTRY_VERIFY_WITH_TIMEOUT(list->width() >= 350, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(metadata->width() >= 170, 2000);
        const int resizedTextHeight = metadata->heightForWidth(metadata->width());
        QVERIFY2(resizedTextHeight > 0
                     && resizedTextHeight <= 7 * metadata->fontMetrics().lineSpacing(),
                 qPrintable(QStringLiteral("Metadata wraps too deeply after resize: width=%1 textHeight=%2")
                                .arg(metadata->width()).arg(resizedTextHeight)));
    }
};

QTEST_MAIN(QuotedPostPreviewAttachmentLayoutTest)
#include "QuotedPostPreviewAttachmentLayoutTest.moc"
