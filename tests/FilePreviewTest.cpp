#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QtTest>

#include "preview-window/FilePreview.h"

using namespace Mattermost;

namespace {

QSize displayedPixmapSize(const QLabel* label)
{
    if (!label) {
        return {};
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return label->pixmap(Qt::ReturnByValue).size();
#else
    const QPixmap* pixmap = label->pixmap();
    return pixmap ? pixmap->size() : QSize();
#endif
}

bool sameAspectRatio(const QSize& size, const QSize& source)
{
    if (size.isEmpty() || source.isEmpty()) {
        return false;
    }

    const qint64 lhs = qint64(size.width()) * source.height();
    const qint64 rhs = qint64(size.height()) * source.width();
    return qAbs(lhs - rhs) <= source.width() + source.height();
}

} // namespace

class FilePreviewTest : public QObject
{
    Q_OBJECT

private slots:
    void resizeFitsOriginalWithoutProgressiveShrink()
    {
        const QSize sourceSize(1200, 600);
        QImage image(sourceSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);

        FilePreview preview(
            image,
            QStringLiteral("preview.png"),
            QStringLiteral("Alice"));

        auto* imageLabel =
            preview.findChild<QLabel*>(QStringLiteral("fileContents"));
        QVERIFY(imageLabel);
        QCOMPARE(imageLabel->frameShape(), QFrame::NoFrame);

        preview.show();
        QApplication::processEvents();

        const QSize initial = displayedPixmapSize(imageLabel);
        QVERIFY(initial.width() > 1);
        QVERIFY(initial.height() > 1);
        QVERIFY(sameAspectRatio(initial, sourceSize));
        QVERIFY(initial.width() <= sourceSize.width());
        QVERIFY(initial.height() <= sourceSize.height());

        preview.resize(500, 320);
        QApplication::processEvents();
        QCOMPARE(imageLabel->size(), preview.size());
        const QSize small = displayedPixmapSize(imageLabel);
        QVERIFY(small.width() > 1);
        QVERIFY(small.height() > 1);
        QVERIFY(sameAspectRatio(small, sourceSize));
        QCOMPARE(small, QSize(500, 250));

        preview.resize(900, 600);
        QApplication::processEvents();
        QCOMPARE(imageLabel->size(), preview.size());
        const QSize large = displayedPixmapSize(imageLabel);
        QVERIFY(large.width() > small.width());
        QVERIFY(large.height() > small.height());
        QVERIFY(sameAspectRatio(large, sourceSize));
        QCOMPARE(large, QSize(900, 450));

        // Re-applying the same window size must produce the same image size.
        // This guards against the old feedback loop where every resize scaled
        // an already-shrunk geometry again.
        preview.resize(900, 600);
        QApplication::processEvents();
        QCOMPARE(displayedPixmapSize(imageLabel), large);
    }
};

QTEST_MAIN(FilePreviewTest)
#include "FilePreviewTest.moc"
