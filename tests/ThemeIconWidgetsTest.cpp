#include <QtTest>

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPalette>

#include "ui/ThemeIconWidgets.h"

using namespace Mattermost;

namespace {
int averageLuma(const QImage& image)
{
    qint64 sum = 0;
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() < 32) {
                continue;
            }
            sum += color.red() + color.green() + color.blue();
            count += 3;
        }
    }
    return count ? static_cast<int>(sum / count) : -1;
}

QImage renderButton(ThemeIconButton& button)
{
    QImage image(button.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    button.render(&painter);
    return image;
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
        const int darkLuma = averageLuma(renderButton(button));

        QPalette light = original;
        light.setColor(QPalette::ButtonText, Qt::black);
        qApp->setPalette(light);
        QCoreApplication::processEvents();
        const int lightLuma = averageLuma(renderButton(button));

        qApp->setPalette(original);
        QVERIFY(darkLuma >= 180);
        QVERIFY(lightLuma >= 0 && lightLuma <= 75);
    }
};

QTEST_MAIN(ThemeIconWidgetsTest)
#include "ThemeIconWidgetsTest.moc"
