#include <QtTest>

#include <QImage>
#include <QTemporaryDir>

#include "ui/EmojiRaster.h"

using namespace Mattermost;

class EmojiSmoothScalingTest final : public QObject
{
    Q_OBJECT

private slots:
    void downsamplesCustomEmojiWithAntialiasing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        // Alternating one-pixel black/white lines must blend into greys.
        // Nearest-neighbour shrinking keeps only black or white pixels.
        QImage original(63, 63, QImage::Format_ARGB32);
        for (int y = 0; y < original.height(); ++y) {
            for (int x = 0; x < original.width(); ++x) {
                const int intensity = x % 2 ? 255 : 0;
                original.setPixelColor(x, y, QColor(intensity, intensity,
                                                    intensity, 255));
            }
        }
        // CustomEmojiService may store PNG bytes in a file with a GIF suffix.
        const QString input = directory.filePath(QStringLiteral("custom.gif"));
        QVERIFY(original.save(input, "PNG"));

        const QString thumb = EmojiRaster::smoothedPath(input, QSize(16, 16));
        QVERIFY2(!thumb.isEmpty(), "Expected a thumbnail for an oversized image");
        const QImage downsampled(thumb);
        QCOMPARE(downsampled.size(), QSize(16, 16));
        const int centre = downsampled.pixelColor(8, 8).red();
        QVERIFY2(centre > 20 && centre < 235,
                 "Smooth resampling should blend alternating pixel columns");

        // No redundant reads/rewrites at the same target size.
        QCOMPARE(EmojiRaster::smoothedPath(input, QSize(16, 16)), thumb);
        QCOMPARE(EmojiRaster::smoothedPath(input, QSize(63, 63)), QString());
    }

    void preservesAspectRatioAndRejectsRemoteSources()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QImage vertical(30, 90, QImage::Format_ARGB32);
        vertical.fill(Qt::red);
        const QString input = directory.filePath(QStringLiteral("vertical.png"));
        QVERIFY(vertical.save(input, "PNG"));
        const QString thumb = EmojiRaster::smoothedPath(input, QSize(16, 16));
        QVERIFY(!thumb.isEmpty());
        QCOMPARE(QImage(thumb).size(), QSize(5, 16));

        QVERIFY(EmojiRaster::smoothedPath(
            QStringLiteral("https://example.org/image.png"), QSize(16, 16)).isEmpty());
        QVERIFY(EmojiRaster::smoothedPath(QString(), QSize(16, 16)).isEmpty());
    }
};

QTEST_GUILESS_MAIN(EmojiSmoothScalingTest)
#include "EmojiSmoothScalingTest.moc"
