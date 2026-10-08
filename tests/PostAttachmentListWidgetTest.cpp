#include <QtTest>

#include <QListWidgetItem>
#include <QLayout>
#include <QLabel>
#include <QFontMetrics>

#include "ui_AttachedBinaryFile.h"
#include <QWidget>

#include "chat-area/post/attachments/PostAttachmentListWidget.h"

using namespace Mattermost;

namespace {

void settleEvents()
{
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
    }
}

} // namespace

class PostAttachmentListWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void attachmentMetadataUsesConsistentTextLayout()
    {
        QWidget card;
        Ui::AttachedBinaryFile ui;
        ui.setupUi(&card);

        ui.metadataLabel->setTextFormat(Qt::PlainText);
        ui.metadataLabel->setText(
            QStringLiteral("File: tl_tools_installer.sh\\n"
                           "Type: application/x-shellscript\\n"
                           "Size: 158.32 kB"));
        ui.downloadedLabel->setText(
            QStringLiteral("/home/silinykh/Downloads/tl_tools_installer.sh"));
        card.show();
        settleEvents();

        // One text layout owns all three baselines; three independent labels
        // used to inherit different heights after width-dependent reflow.
        QCOMPARE(ui.metadataLabel->text().count(QLatin1Char('\\n')), 2);
        const int wide = ui.metadataLabel->heightForWidth(600);
        const int narrow = ui.metadataLabel->heightForWidth(130);
        QVERIFY2(wide > 0, "Metadata must advertise height-for-width");
        QVERIFY2(narrow > wide, "Wrapping at narrow post width must increase height");
        const int lineHeight = ui.metadataLabel->fontMetrics().lineSpacing();
        QVERIFY2(wide <= 3 * lineHeight + 12,
                 qPrintable(QStringLiteral("Unexpected empty line in metadata: height=%1 line=%2")
                              .arg(wide).arg(lineHeight)));

        QVERIFY2(card.layout()->hasHeightForWidth(),
                 "Attachment card must propagate height-for-width to the list");
        QVERIFY(card.layout()->heightForWidth(250)
                > card.layout()->heightForWidth(700));
    }

    void singleItemSizeHintIncludesOuterSpacing()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);

        auto* item = new QListWidgetItem(&list);
        item->setSizeHint(QSize(320, 500));

        QCOMPARE(list.sizeHint(), QSize(340, 520));
    }

    void multipleItemsIncludeOuterAndInterItemSpacing()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);

        auto* first = new QListWidgetItem(&list);
        first->setSizeHint(QSize(320, 500));
        auto* second = new QListWidgetItem(&list);
        second->setSizeHint(QSize(180, 100));

        QCOMPARE(list.sizeHint(), QSize(340, 630));
    }

    void sizeHintKeepsPreviewInsideViewport()
    {
        PostAttachmentListWidget list;
        list.setFrameShape(QFrame::NoFrame);
        list.setSpacing(10);
        list.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto* item = new QListWidgetItem(&list);
        item->setSizeHint(QSize(500, 300));

        auto* preview = new QWidget;
        preview->setFixedSize(500, 300);
        list.setItemWidget(item, preview);

        list.resize(list.sizeHint());
        list.show();
        settleEvents();

        const QRect viewportRect = list.viewport()->rect();
        const QRect itemRect = list.visualItemRect(item);

        QVERIFY2(viewportRect.contains(itemRect),
                 qPrintable(QStringLiteral(
                     "Attachment item must fit inside the viewport: viewport=%1,%2 %3x%4; item=%5,%6 %7x%8")
                     .arg(viewportRect.x()).arg(viewportRect.y())
                     .arg(viewportRect.width()).arg(viewportRect.height())
                     .arg(itemRect.x()).arg(itemRect.y())
                     .arg(itemRect.width()).arg(itemRect.height())));

        QCOMPARE(itemRect.topLeft(), QPoint(10, 10));
        QCOMPARE(itemRect.size(), QSize(500, 300));
        QCOMPARE(viewportRect.right() - itemRect.right(), 10);
        QCOMPARE(viewportRect.bottom() - itemRect.bottom(), 10);
    }
};

QTEST_MAIN(PostAttachmentListWidgetTest)

#include "PostAttachmentListWidgetTest.moc"
