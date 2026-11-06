#include <QtTest>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QGridLayout>
#include <QUuid>
#include <QSlider>
#include "ui/FontSettingsWidget.h"

#include "Settings.h"
#include "options/MLOptions.h"
#include "ui/ApplicationFont.h"

using namespace Mattermost;

class ApplicationFontTest final : public QObject
{
    Q_OBJECT
    QFont systemFont;

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("MatterLeast-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("ApplicationFontTest-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces));
        systemFont = QApplication::font();
    }

    void restoresSavedSizeAndUpdatesExistingAndNewWidgets()
    {
        auto* options = MLOptions::instance();
        QFont legacyChat = systemFont;
        legacyChat.setPointSize(13);
        legacyChat.setItalic(true);
        options->setValue<QString>(QStringLiteral("chat/font"), legacyChat.toString());
        options->setValue<int>(QStringLiteral("ui/fontPointSize"), 18);
        QLabel existing(QStringLiteral("Existing interface"));
        initializeApplicationFont(*qApp);
        QCoreApplication::processEvents();
        QCOMPARE(options->value<QString>(CHAT_FONT), legacyChat.toString());
        auto* size = options->optionObject<QString>(INTERFACE_FONT, QString());
        QFont changed = systemFont;
        QCOMPARE(QApplication::font().pointSize(), 18);
        QCOMPARE(existing.font().pointSize(), 18);
        QCOMPARE(QApplication::font().family(), systemFont.family());

        changed.setPointSize(24);
        changed.setItalic(true);
        size->setValue(changed.toString());
        QCoreApplication::processEvents();
        QCOMPARE(existing.font().pointSize(), 24);
        QVERIFY(existing.font().italic());
        QLabel createdLater;
        QCOMPARE(createdLater.font().pointSize(), 24);

        // Settings Cancel restores its snapshot via the same option signal.
        changed.setPointSize(18);
        size->setValue(changed.toString());
        QCoreApplication::processEvents();
        QCOMPARE(existing.font().pointSize(), 18);

        size->setValue(QString());
        QCoreApplication::processEvents();
        QFont defaultFont = systemFont;
        defaultFont.setPointSize(11);
        QCOMPARE(existing.font(), defaultFont);
        QCOMPARE(QApplication::font(), defaultFont);
    }
    void genericRowsKeepFontStateAndEmitTheirOwnIds()
    {
        QFont initial = systemFont;
        initial.setPointSizeF(12.5);
        initial.setBold(true);
        FontSettingsWidget editor({
            {QStringLiteral("chat"), QStringLiteral("Chat"), initial.toString()},
            {QStringLiteral("interface"), QStringLiteral("Interface"), systemFont.toString()},
            {QStringLiteral("extra"), QStringLiteral("Third font"), initial.toString()}
        });
        auto* grid = qobject_cast<QGridLayout*>(editor.layout());
        QVERIFY(grid);
        QCOMPARE(grid->rowCount(), 3);
        QCOMPARE(grid->columnCount(), 4);
        QSignalSpy changes(&editor, &FontSettingsWidget::fontChanged);
        auto* family = editor.findChild<QFontComboBox*>(QStringLiteral("chatFontFamily"));
        auto* size = editor.findChild<QDoubleSpinBox*>(QStringLiteral("chatFontSize"));
        auto* slider = editor.findChild<QSlider*>(QStringLiteral("chatFontSizeSlider"));
        QCOMPARE(grid->itemAtPosition(0, 1)->widget(), family);
        QCOMPARE(grid->itemAtPosition(0, 2)->widget(), size);
        QCOMPARE(grid->itemAtPosition(0, 3)->widget(), slider);
        QCOMPARE(size->value(), 12.5);
        QCOMPARE(changes.count(), 0);
        slider->setValue(20);
        QCOMPARE(size->value(), 20.0);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(changes.last().at(0).toString(), QStringLiteral("chat"));
        QFont result;
        QVERIFY(result.fromString(changes.last().at(1).toString()));
        QCOMPARE(result.pointSize(), 20);
        QVERIFY(result.bold());
        size->setValue(17.5);
        QCOMPARE(changes.count(), 2);
        QCOMPARE(slider->value(), 18);
        QVERIFY(result.fromString(changes.last().at(1).toString()));
        QCOMPARE(result.pointSizeF(), 17.5);
        auto* extra = editor.findChild<QSlider*>(QStringLiteral("extraFontSizeSlider"));
        extra->setValue(21);
        QCOMPARE(changes.last().at(0).toString(), QStringLiteral("extra"));
        QCOMPARE(size->value(), 17.5);
    }

};

QTEST_MAIN(ApplicationFontTest)
#include "ApplicationFontTest.moc"
