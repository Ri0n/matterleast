#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QScrollArea>
#include <QtTest>

#include "preview-window/FilePreview.h"

using namespace Mattermost;

class FilePreviewTest : public QObject
{
    Q_OBJECT

private slots:
    void dialogResizeDoesNotShrinkImage()
    {
        QImage image(640, 480, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);

        FilePreview preview(
            image,
            QStringLiteral("preview.png"),
            QStringLiteral("Alice"));

        auto* imageLabel =
            preview.findChild<QLabel*>(QStringLiteral("fileContents"));
        auto* scrollArea = preview.findChild<QScrollArea*>();
        QVERIFY(imageLabel);
        QVERIFY(scrollArea);

        preview.show();
        QApplication::processEvents();

        const QSize initialImageSize = imageLabel->size();
        QVERIFY(initialImageSize.width() > 1);
        QVERIFY(initialImageSize.height() > 1);

        // FilePreview intentionally keeps the image geometry chosen when the
        // window opens. Shrinking the viewport should expose scrollbars rather
        // than rescaling the image to a transient layout size.
        scrollArea->setMinimumSize(1, 1);
        preview.resize(240, 180);
        QApplication::processEvents();

        QCOMPARE(imageLabel->size(), initialImageSize);
    }
};

QTEST_MAIN(FilePreviewTest)
#include "FilePreviewTest.moc"
