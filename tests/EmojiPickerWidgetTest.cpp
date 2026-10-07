#include <optional>

#include <QApplication>
#include <QPushButton>
#include <QStackedWidget>
#include <QtTest>

#include "Settings.h"
#include "backend/Backend.h"
#include "backend/emoji/EmojiInfo.h"
#include "choose-emoji-dialog/EmojiPickerWidget.h"
#include "options/MLOptions.h"

using namespace Mattermost;

class EmojiPickerWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void appliesDefaultToneAndOffersLongPressOverride()
    {
        auto* options = MLOptions::instance();
        const int previousTone = options->value<int>(
            EMOJI_DEFAULT_SKIN_TONE,
            EMOJI_DEFAULT_SKIN_TONE_DEFAULT);
        options->setValue<int>(
            EMOJI_DEFAULT_SKIN_TONE,
            EmojiSkinTone::medium);

        Backend backend;
        EmojiPickerWidget picker(backend);
        picker.prepare();
        picker.resize(560, 420);
        picker.show();
        QVERIFY(QTest::qWaitForWindowExposed(&picker));
        QApplication::processEvents();

        auto* stack = picker.findChild<QStackedWidget*>();
        QVERIFY(stack);
        // People is the second visible emoji category (Component has no tab).
        stack->setCurrentIndex(1);
        QApplication::processEvents();

        const auto variants =
            EmojiInfo::skinToneVariantsByName(QStringLiteral("+1"));
        QCOMPARE(static_cast<int>(variants.size()),
                 static_cast<int>(EmojiSkinTone::COUNT));
        QVERIFY(EmojiInfo::skinToneVariantsByName(
                    QStringLiteral("eyes")).isEmpty());
        QVERIFY(!EmojiInfo::resolveBuiltInByName(
            QStringLiteral("eyes_medium_skin_tone")));

        QPushButton* thumb = nullptr;
        const auto buttons = picker.findChildren<QPushButton*>();
        for (QPushButton* button : buttons) {
            if (button->property("mattermostEmojiBaseName").toString()
                == QStringLiteral("+1")) {
                thumb = button;
                break;
            }
        }
        QVERIFY(thumb);
        QCOMPARE(thumb->text(), QString::fromUtf8("👍🏽"));

        std::optional<Emoji> chosen;
        connect(&picker, &EmojiPickerWidget::emojiChosen,
                this, [&chosen](const Emoji& emoji) {
            chosen = emoji;
        });

        QTest::mouseClick(thumb, Qt::LeftButton);
        QVERIFY(chosen);
        QCOMPARE(chosen->name, QStringLiteral("+1_medium_skin_tone"));
        QCOMPARE(chosen->unicodeString, QString::fromUtf8("👍🏽"));

        chosen.reset();
        QTest::mousePress(thumb, Qt::LeftButton);

        const auto findSkinTonePopup = []() -> QWidget* {
            const auto topLevels = QApplication::topLevelWidgets();
            for (QWidget* widget : topLevels) {
                if (widget
                    && widget->objectName()
                        == QStringLiteral("emojiSkinTonePopup")) {
                    return widget;
                }
            }
            return nullptr;
        };

        QTRY_VERIFY_WITH_TIMEOUT(
            findSkinTonePopup() != nullptr,
            QApplication::startDragTime() + 1000);
        QWidget* popup = findSkinTonePopup();
        QVERIFY(popup);

        const auto toneButtons =
            popup->findChildren<QPushButton*>(
                QStringLiteral("emojiSkinToneOption"));
        QCOMPARE(static_cast<int>(toneButtons.size()),
                 static_cast<int>(EmojiSkinTone::COUNT));

        QPushButton* dark = nullptr;
        for (QPushButton* button : toneButtons) {
            if (button->text() == QString::fromUtf8("👍🏿")) {
                dark = button;
                break;
            }
        }
        QVERIFY(dark);
        QTest::mouseClick(dark, Qt::LeftButton);
        QVERIFY(chosen);
        QCOMPARE(chosen->name, QStringLiteral("+1_dark_skin_tone"));
        QCOMPARE(chosen->unicodeString, QString::fromUtf8("👍🏿"));

        QCOMPARE(options->value<int>(
                     EMOJI_DEFAULT_SKIN_TONE,
                     EMOJI_DEFAULT_SKIN_TONE_DEFAULT),
                 static_cast<int>(EmojiSkinTone::medium));

        options->setValue<int>(EMOJI_DEFAULT_SKIN_TONE, previousTone);
    }
};

QTEST_MAIN(EmojiPickerWidgetTest)
#include "EmojiPickerWidgetTest.moc"
