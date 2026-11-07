#include <QtTest>

#include <QApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QWidget>

#include "ui/SvgRasterCache.h"
#include "ui/IconUtils.h"
#include "ui/TrayBadgePainter.h"
#include "ui/TrayBadgePolicy.h"
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
    void trayBadgePriorityAndChannelEligibility()
    {
        QVERIFY(trayBadgeKind(0, false) == TrayBadgeKind::None);
        QVERIFY(trayBadgeKind(0, true) == TrayBadgeKind::ChannelUnread);
        QVERIFY(trayBadgeKind(1, false) == TrayBadgeKind::Attention);
        QVERIFY(trayBadgeKind(12, true) == TrayBadgeKind::Attention);

        QVERIFY(isTrayUnreadChannel(true, true, false, true));
        QVERIFY(!isTrayUnreadChannel(false, true, false, true)); // DM/GM
        QVERIFY(!isTrayUnreadChannel(true, false, false, true)); // untracked
        QVERIFY(!isTrayUnreadChannel(true, true, true, true));  // muted
        QVERIFY(!isTrayUnreadChannel(true, true, false, false)); // read
    }

    void trayBadgesCutAnActualTransparentHalo()
    {
        // A fully opaque synthetic source proves that the ring removes icon
        // pixels instead of painting an arbitrary light/dark outline.
        for (int size : {16, 32, 64, 128}) {
            for (TrayBadgeKind kind :
                 {TrayBadgeKind::Attention, TrayBadgeKind::ChannelUnread}) {
                QPixmap pixmap(size, size);
                const QColor background(50, 75, 90);
                pixmap.fill(background);
                TrayBadgePainter::paint(pixmap, kind, 18);
                const QImage result = pixmap.toImage();

                QCOMPARE(result.pixelColor(0, 0), background);
                int clearCount = 0;
                int redCount = 0;
                int blueCount = 0;
                // The halo extends to the image edge at tiny 16px sizes.
                // Scan the whole originally-opaque image; antialiasing can
                // produce a partially cleared rim rather than a zero-alpha
                // center pixel at the smallest resolution.
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const QColor color = result.pixelColor(x, y);
                        if (color.alpha() < 128) {
                            ++clearCount;
                        }
                        if (color.alpha() > 200 && color.red() > 170
                            && color.red() > color.blue() + 60) {
                            ++redCount;
                        }
                        if (color.alpha() > 200 && color.blue() > 170
                            && color.blue() > color.red() + 60) {
                            ++blueCount;
                        }
                    }
                }

                QVERIFY2(clearCount > 0, qPrintable(
                    QStringLiteral("No alpha-cutout ring at %1px (kind=%2)")
                        .arg(size).arg(static_cast<int>(kind))));
                if (kind == TrayBadgeKind::Attention) {
                    QVERIFY(redCount > 0);
                    QCOMPARE(blueCount, 0);
                } else {
                    QVERIFY(blueCount > 0);
                    QCOMPARE(redCount, 0);
                }
            }
        }
    }

    void trayBadgePainterKeepsLogicalGeometryAtHighDpi()
    {
        QPixmap pixmap(128, 128);
        pixmap.setDevicePixelRatio(2.0);
        const QColor background(50, 75, 90);
        pixmap.fill(background);
        TrayBadgePainter::paint(pixmap, TrayBadgeKind::ChannelUnread);
        const QImage result = pixmap.toImage();

        QCOMPARE(result.pixelColor(0, 0), background);
        int cutoutPixels = 0;
        for (int y = 64; y < result.height(); ++y) {
            for (int x = 64; x < result.width(); ++x) {
                if (result.pixelColor(x, y).alpha() == 0) {
                    ++cutoutPixels;
                }
            }
        }
        QVERIFY(cutoutPixels > 0);
    }

    void applicationTrayIconUsesOnlyOneBadge()
    {
        const QIcon normal = IconUtils::applicationIcon();
        const QIcon blue = IconUtils::applicationIcon(0, true);
        const QIcon red = IconUtils::applicationIcon(4, true);
        const QIcon redOnly = IconUtils::applicationIcon(4, false);
        QVERIFY(!normal.isNull());
        QVERIFY(!blue.isNull());
        QVERIFY(!red.isNull());
        QVERIFY(!redOnly.isNull());

        const QImage normalImage = normal.pixmap(64, 64).toImage();
        const QImage blueImage = blue.pixmap(64, 64).toImage();
        const QImage redImage = red.pixmap(64, 64).toImage();
        const QImage redOnlyImage = redOnly.pixmap(64, 64).toImage();
        QVERIFY(normalImage != blueImage);
        QVERIFY(blueImage != redImage);
        // A pending unread channel must never leak a second dot when
        // personal attention is also present.
        QCOMPARE(redImage, redOnlyImage);
    }

    void iconsRenderWithoutAnyImageformatPlugins()
    {
        // Reproduce a minimal Debian/Qt installation with no qsvg plugin.
        // The QPA plugin is already loaded by QApplication at this point.
        const QStringList originalPaths = QCoreApplication::libraryPaths();
        QCoreApplication::setLibraryPaths({});
        const QStringList resources {
            QStringLiteral("emoji"), QStringLiteral("message-balloon"),
            QStringLiteral("bookmark"), QStringLiteral("more-horizontal"),
            QStringLiteral("link"),
            QStringLiteral("image"), QStringLiteral("format-bullet-list"),
            QStringLiteral("format-numbered-list"),
            QStringLiteral("message-priority"), QStringLiteral("add"),
            QStringLiteral("search"), QStringLiteral("members"),
            QStringLiteral("burger"), QStringLiteral("filter-variant"),
            QStringLiteral("pin")
        };
        for (const QString& name : resources) {
            const QString path = QStringLiteral(":/icons/") + name;
            const QIcon icon = IconUtils::tintedSymbolicIcon(path, Qt::black);
            QVERIFY2(!icon.isNull(), qPrintable(path));
            const QImage rendered = icon.pixmap(24, 24).toImage();
            QVERIFY2(!rendered.isNull(), qPrintable(path));
            bool hasVisiblePixel = false;
            for (int y = 0; y < rendered.height() && !hasVisiblePixel; ++y) {
                for (int x = 0; x < rendered.width(); ++x) {
                    if (rendered.pixelColor(x, y).alpha() > 100) {
                        hasVisiblePixel = true;
                        break;
                    }
                }
            }
            QVERIFY2(hasVisiblePixel, qPrintable(path));
        }
        const QPixmap svg = SvgRasterCache::instance().raster(
            QStringLiteral(":/icons/link"), QSize(20, 20), 1.5);
        QVERIFY(!svg.isNull());
        QCoreApplication::setLibraryPaths(originalPaths);
    }

    void unreadFilterUsesCompassSvgInsteadOfSystemFonts()
    {
        const QString path = QStringLiteral(":/icons/filter-variant");
        const QIcon original(path);
        QVERIFY2(!original.isNull(), "Compass filter-variant missing from resource.qrc");

        const QImage shape = original.pixmap(24, 24).toImage();
        QVERIFY(!shape.isNull());
        // Exact Mattermost Compass F0236 geometry: horizontal bars of
        // decreasing widths (18, 12 and 4 units), not an envelope/info icon.
        QVERIFY(shape.pixelColor(4, 7).alpha() > 200);
        QVERIFY(shape.pixelColor(7, 12).alpha() > 200);
        QVERIFY(shape.pixelColor(11, 17).alpha() > 200);
        QVERIFY(shape.pixelColor(6, 17).alpha() < 10);
        QVERIFY(shape.pixelColor(20, 12).alpha() < 10);

        const QColor inactive(32, 72, 104);
        const QColor active(215, 101, 30);
        const QIcon normal = IconUtils::tintedSymbolicIcon(path, inactive);
        const QIcon highlighted = IconUtils::tintedSymbolicIcon(path, active);
        QVERIFY(!normal.isNull());
        QVERIFY(!highlighted.isNull());
        QCOMPARE(normal.pixmap(24, 24).toImage().pixelColor(11, 7), inactive);
        QCOMPARE(highlighted.pixmap(24, 24).toImage().pixelColor(11, 7), active);
    }

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

    void narrowFormattingToolbarWrapsWithoutOverlap()
    {
        QWidget toolbar;
        toolbar.setObjectName(QStringLiteral("formattingToolbar"));
        toolbar.resize(58, 100);

        auto* originalLayout = new QHBoxLayout(&toolbar);
        originalLayout->setContentsMargins(0, 0, 0, 0);
        originalLayout->setSpacing(1);

        auto makeButton = [&toolbar, originalLayout](const QString& name,
                                                     const QString& text) {
            auto* button = new ThemeIconButton(&toolbar);
            button->setObjectName(name);
            button->setText(text);
            originalLayout->addWidget(button);
            return button;
        };

        ThemeIconButton* first = makeButton(
            QStringLiteral("formatBoldButton"), QStringLiteral("B"));
        ThemeIconButton* second = makeButton(
            QStringLiteral("formatItalicButton"), QStringLiteral("I"));
        ThemeIconButton* third = makeButton(
            QStringLiteral("formatStrikeButton"), QStringLiteral("S"));

        toolbar.show();
        QCoreApplication::processEvents();
        toolbar.layout()->setGeometry(toolbar.rect());
        QCoreApplication::processEvents();

        QVERIFY(toolbar.layout()->hasHeightForWidth());
        QVERIFY(toolbar.sizePolicy().hasHeightForWidth());

        // Font metrics differ slightly between the Qt 5 and Qt 6 CI images.
        // At this deliberately narrow width Qt 6 can fit two 28px controls on
        // the first row, while Qt 5 may legitimately wrap after every control.
        // The layout contract is therefore non-overlap plus monotonic wrapping,
        // not a particular number of controls on row one.
        QVERIFY(second->geometry().y() >= first->geometry().y());
        QVERIFY(third->geometry().y() >= second->geometry().y());
        QVERIFY(second->geometry().y() > first->geometry().y()
                || third->geometry().y() > first->geometry().y());
        QVERIFY(!first->geometry().intersects(second->geometry()));
        QVERIFY(!first->geometry().intersects(third->geometry()));
        QVERIFY(!second->geometry().intersects(third->geometry()));
    }
};

QTEST_MAIN(ThemeIconWidgetsTest)
#include "ThemeIconWidgetsTest.moc"
