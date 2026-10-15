#include <QtTest/QtTest>

#include <QDate>
#include <QImage>
#include <QLibrary>
#include <QPluginLoader>
#include <QPalette>
#include <QWidget>
#include <QTimeZone>

#include "plugins/api/MainWindowOverlayInterface.h"
#include "plugins/api/PluginInterface.h"
#include "plugins/api/PluginSettingsInterface.h"
#include "plugins/seasonal-snow/SnowSeason.h"
#include "plugins/seasonal-snow/SnowSettings.h"

class SeasonalSnowTest final : public QObject
{
    Q_OBJECT

private slots:
    void activatesOnlyDuringHolidaySeason()
    {
        using MatterLeast::SeasonalSnow::activeOn;
        QVERIFY(!activeOn(QDate(2026, 12, 23)));
        QVERIFY(activeOn(QDate(2026, 12, 24)));
        QVERIFY(activeOn(QDate(2026, 12, 31)));
        QVERIFY(activeOn(QDate(2027, 1, 1)));
        QVERIFY(activeOn(QDate(2027, 1, 7)));
        QVERIFY(!activeOn(QDate(2027, 1, 8)));
        QVERIFY(!activeOn(QDate(2026, 10, 14)));
        QVERIFY(!activeOn(QDate()));
    }

    void schedulesDailyLocalDateCheck()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QDateTime now(QDate(2026, 12, 24), QTime(11, 30),
                            QTimeZone::systemTimeZone());
#else
        const QDateTime now(QDate(2026, 12, 24), QTime(11, 30),
                            Qt::LocalTime);
#endif
        const int remaining =
            MatterLeast::SeasonalSnow::untilNextLocalMidnight(now);
        QVERIFY(remaining > 11 * 3600 * 1000);
        QVERIFY(remaining <= 13 * 3600 * 1000);
    }

    void snowSettingsScaleSizeAndPopulation()
    {
        using namespace MatterLeast::SeasonalSnow;
        QCOMPARE(DefaultSizePercent, 150);
        QCOMPARE(DefaultIntensityPercent, 100);
        QCOMPARE(radiusScale(DefaultSizePercent), 1.5);
        QCOMPARE(flakeCountForArea(800, 600, 100), 9);
        QCOMPARE(flakeCountForArea(800, 600, 200), 18);
        QCOMPARE(flakeCountForArea(800, 600, 25), 2);
        QCOMPARE(flakeCountForArea(0, 600, 250), 0);
    }

    void overlayStaysClickThroughAndCanBeUnloaded()
    {
        const QString path =
            qEnvironmentVariable("MATTERLEAST_SNOW_TEST_PLUGIN_PATH");
        QVERIFY2(!path.isEmpty(), "Missing snow plugin test path");

        qputenv("MATTERLEAST_SNOW_PREVIEW", "1");
        QPluginLoader loader(path);
        loader.setLoadHints(loader.loadHints() & ~QLibrary::PreventUnloadHint);
        QObject* object = loader.instance();
        QVERIFY2(object, qPrintable(loader.errorString()));

        auto* plugin = qobject_cast<
            MatterLeast::PluginApi::PluginInterface*>(object);
        auto* overlayExtension = qobject_cast<
            MatterLeast::PluginApi::MainWindowOverlayInterface*>(object);
        auto* settings = qobject_cast<
            MatterLeast::PluginApi::PluginSettingsInterface*>(object);
        QVERIFY(plugin);
        QVERIFY(overlayExtension);
        QVERIFY(settings);
        const auto descriptors = settings->settingDescriptors();
        QCOMPARE(descriptors.size(), 2);
        QCOMPARE(descriptors.at(0).value(QStringLiteral("key")).toString(),
                 QStringLiteral("sizePercent"));
        QCOMPARE(descriptors.at(0).value(QStringLiteral("default")).toInt(), 150);
        QCOMPARE(descriptors.at(1).value(QStringLiteral("key")).toString(),
                 QStringLiteral("intensityPercent"));
        QCOMPARE(descriptors.at(1).value(QStringLiteral("default")).toInt(), 100);
        QVERIFY(plugin->enable());

        QWidget mainWindow;
        mainWindow.resize(800, 600);
        QPalette windowPalette = mainWindow.palette();
        windowPalette.setColor(QPalette::Window, QColor(31, 122, 79));
        mainWindow.setPalette(windowPalette);
        mainWindow.setAutoFillBackground(true);

        // Two different surfaces emulate a styled chat panel over a window
        // background. The overlay must not obscure either sibling content.
        QWidget chatPanel(&mainWindow);
        chatPanel.setGeometry(80, 75, 630, 435);
        QPalette panelPalette = chatPanel.palette();
        panelPalette.setColor(QPalette::Window, QColor(57, 85, 168));
        chatPanel.setPalette(panelPalette);
        chatPanel.setAutoFillBackground(true);
        chatPanel.show();
        mainWindow.show();
        const QImage beforeSnow = mainWindow.grab().toImage().convertToFormat(
            QImage::Format_RGB32);
        QVERIFY(!beforeSnow.isNull());

        overlayExtension->attachToMainWindow(&mainWindow);

        auto* overlay = mainWindow.findChild<QWidget*>(
            QStringLiteral("seasonalSnowOverlay"));
        QVERIFY(overlay);
        QVERIFY(overlay->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(overlay->geometry(), mainWindow.rect());
        QTRY_VERIFY(overlay->isVisible());
        QCOMPARE(overlay->property("snowflakeSizePercent").toInt(), 150);
        const int originalCount = overlay->property("snowflakeCount").toInt();
        QCOMPARE(originalCount, MatterLeast::SeasonalSnow::flakeCountForArea(800, 600, 100));
        QVERIFY(settings->setSettingValue(QStringLiteral("sizePercent"), 225));
        QCOMPARE(overlay->property("snowflakeSizePercent").toInt(), 225);
        QVERIFY(settings->setSettingValue(QStringLiteral("intensityPercent"), 200));
        QCOMPARE(overlay->property("snowflakeCount").toInt(), originalCount * 2);
        QVERIFY(!settings->setSettingValue(QStringLiteral("sizePercent"), 999));
        QVERIFY(!settings->setSettingValue(QStringLiteral("unknown"), 100));

        // Regress the black-window bug: clearing alpha in the shared QWidget
        // backing store used to blank the entire main window behind the snow.
        // A sparse animated flake may change individual pixels, but almost all
        // of the parent and chat panel must remain identical to the baseline.
        const QImage withSnow = mainWindow.grab().toImage().convertToFormat(
            QImage::Format_RGB32);
        QCOMPARE(withSnow.size(), beforeSnow.size());
        int samples = 0;
        int unchanged = 0;
        for (int y = 8; y < withSnow.height(); y += 17) {
            for (int x = 8; x < withSnow.width(); x += 17) {
                ++samples;
                if (withSnow.pixel(x, y) == beforeSnow.pixel(x, y)) {
                    ++unchanged;
                }
            }
        }
        QVERIFY2(unchanged * 100 >= samples * 90,
                 "Snow overlay blanked or repainted the underlying window");

        // A transparent overlay must never be the recipient of a mouse hit.
        QCOMPARE(mainWindow.childAt(QPoint(200, 200)), &chatPanel);

        mainWindow.resize(950, 700);
        QTRY_COMPARE(overlay->geometry(), mainWindow.rect());
        QVERIFY(plugin->disable());
        QVERIFY(mainWindow.findChild<QWidget*>(
            QStringLiteral("seasonalSnowOverlay")) == nullptr);
        QVERIFY(loader.unload());
        qunsetenv("MATTERLEAST_SNOW_PREVIEW");
    }
};

QTEST_MAIN(SeasonalSnowTest)
#include "SeasonalSnowTest.moc"
