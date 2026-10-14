#include <QtTest/QtTest>

#include <QDate>
#include <QLibrary>
#include <QPluginLoader>
#include <QWidget>
#include <QTimeZone>

#include "plugins/api/MainWindowOverlayInterface.h"
#include "plugins/api/PluginInterface.h"
#include "plugins/seasonal-snow/SnowSeason.h"

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
        QVERIFY(plugin);
        QVERIFY(overlayExtension);
        QVERIFY(plugin->enable());

        QWidget mainWindow;
        mainWindow.resize(800, 600);
        mainWindow.show();
        overlayExtension->attachToMainWindow(&mainWindow);

        auto* overlay = mainWindow.findChild<QWidget*>(
            QStringLiteral("seasonalSnowOverlay"));
        QVERIFY(overlay);
        QVERIFY(overlay->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(overlay->geometry(), mainWindow.rect());
        QTRY_VERIFY(overlay->isVisible());
        // A transparent overlay must never be the recipient of a mouse hit.
        QVERIFY(mainWindow.childAt(QPoint(200, 200)) == nullptr);

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
