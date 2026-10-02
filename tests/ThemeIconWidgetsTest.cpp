#include <QtTest>

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPalette>

#include "ui/SvgRasterCache.h"
#include "ui/ThemeIconWidgets.h"

using namespace Mattermost;

namespace {

QImage renderButton(ThemeIconButton& button)
{
    QImage image(button.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    button.render(&painter);
    return image;
}

QPair<int, int> changedPixelLuma(const QImage& first, const QImage& second)
{
    qint64 firstSum = 0;
    qint64 secondSum = 0;
    int componentCount = 0;

    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            const QColor a = first.pixelColor(x, y);
            const QColor b = second.pixelColor(x, y);
            const int delta = qAbs(a.red() - b.red())
                + qAbs(a.green() - b.green())
                + qAbs(a.blue() - b.blue());
            if (delta < 30 || (a.alpha() < 32 && b.alpha() < 32)) {
                continue;
            }

            firstSum += a.red() + a.green() + a.blue();
            secondSum += b.red() + b.green() + b.blue();
            componentCount += 3;
        }
    }

    if (componentCount == 0) {
        return {-1, -1};
    }
    return {static_cast<int>(firstSum / componentCount),
            static_cast<int>(secondSum / componentCount)};
}

} // namespace

class ThemeIconWidgetsTest : public QObject
{
    Q_OBJECT
private slots:
    void symbolicIconTracksApplicationPalette()
    {
        const QPalette original = qApp->palette();
        ThemeIconButton button;
        button.setFixedSize(32, 32);
        button.setIconSize(QSize(24, 24));
        button.setProperty(ThemeIconResourceProperty, QStringLiteral(":/icons/bell"));
        button.show();

        QPalette dark = original;
        dark.setColor(QPalette::ButtonText, Qt::white);
        qApp->setPalette(dark);
        QCoreApplication::processEvents();
        const QImage darkImage = renderButton(button);

        QPalette light = original;
        light.setColor(QPalette::ButtonText, Qt::black);
        qApp->setPalette(light);
        QCoreApplication::processEvents();
        const QImage lightImage = renderButton(button);

        qApp->setPalette(original);

        // Native Qt styles may paint a platform-specific button/window
        // background even though ThemeIconButton itself only paints the
        // symbolic glyph. Compare only pixels that actually changed between
        // the two palette renders, which isolates the palette-sensitive icon
        // from that invariant style chrome on both Qt 5 and Qt 6.
        const auto luma = changedPixelLuma(darkImage, lightImage);
        QVERIFY(luma.first >= 0);
        QVERIFY(luma.second >= 0);
        QVERIFY(luma.first > luma.second + 100);
    }

    void svgRasterCacheUsesFinalDeviceResolution()
    {
        const QSize logicalSize(20, 20);
        auto& cache = SvgRasterCache::instance();

        const QPixmap oneX = cache.raster(
            QStringLiteral(":/icons/link"), logicalSize, 1.0);
        const QPixmap oneAndHalfX = cache.raster(
            QStringLiteral(":/icons/link"), logicalSize, 1.5);
        const QPixmap twoX = cache.raster(
            QStringLiteral(":/icons/link"), logicalSize, 2.0);

        QVERIFY(!oneX.isNull());
        QVERIFY(!oneAndHalfX.isNull());
        QVERIFY(!twoX.isNull());
        QCOMPARE(oneX.size(), QSize(20, 20));
        QCOMPARE(oneAndHalfX.size(), QSize(30, 30));
        QCOMPARE(twoX.size(), QSize(40, 40));
        QCOMPARE(oneX.devicePixelRatioF(), qreal(1.0));
        QCOMPARE(oneAndHalfX.devicePixelRatioF(), qreal(1.5));
        QCOMPARE(twoX.devicePixelRatioF(), qreal(2.0));

        const QPixmap cachedAgain = cache.raster(
            QStringLiteral(":/icons/link"), logicalSize, 1.5);
        QCOMPARE(cachedAgain.cacheKey(), oneAndHalfX.cacheKey());
    }
};

QTEST_MAIN(ThemeIconWidgetsTest)
#include "ThemeIconWidgetsTest.moc"
