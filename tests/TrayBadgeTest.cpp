#include <QtTest>

#include <QIcon>
#include <QImage>
#include <QPixmap>

#include "ui/IconUtils.h"

using namespace Mattermost;

class TrayBadgeTest final : public QObject
{
    Q_OBJECT

private slots:
    void rasterizedBadgeDiffersFromNormalIcon()
    {
        const QIcon plain = IconUtils::applicationIcon(0);
        const QIcon badged = IconUtils::applicationIcon(12);
        QVERIFY(!plain.isNull());
        QVERIFY(!badged.isNull());

        // KDE/StatusNotifierItem/Qt5 use small rasterized icons; verify that
        // the badge exists in the pixmap delivered to QSystemTrayIcon.
        for (int extent : {16, 22, 32}) {
            const QImage base = plain.pixmap(extent, extent).toImage();
            const QImage marked = badged.pixmap(extent, extent).toImage();
            QVERIFY2(!base.isNull(), "Base icon did not rasterize");
            QVERIFY2(!marked.isNull(), "Badged icon did not rasterize");
            QCOMPARE(marked.size(), base.size());
            int changed = 0;
            int redBadgePixels = 0;
            for (int y = 0; y < marked.height(); ++y) {
                for (int x = 0; x < marked.width(); ++x) {
                    if (base.pixel(x,y) != marked.pixel(x,y)) ++changed;
                    const QColor c = marked.pixelColor(x,y);
                    if (c.red() > 170 && c.red() > c.green() * 1.8
                        && c.red() > c.blue() * 1.5
                        && y < marked.height() / 2) ++redBadgePixels;
                }
            }
            QVERIFY2(changed > 0, "Unread badge must change the tray raster");
            QVERIFY2(redBadgePixels >= 2, "Unread badge must paint red pixels in top edge");
        }
    }
};

QTEST_MAIN(TrayBadgeTest)
#include "TrayBadgeTest.moc"
