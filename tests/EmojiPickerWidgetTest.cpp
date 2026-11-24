#include <optional>

#include <QApplication>
#include <QPointer>
#include <QTemporaryDir>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QtTest>

#include "Settings.h"
#include "backend/Backend.h"
#include "backend/emoji/EmojiInfo.h"
#include "choose-emoji-dialog/EmojiPickerWidget.h"
#include "choose-emoji-dialog/ChooseEmojiDialog.h"
#include "options/MLOptions.h"

using namespace Mattermost;

class EmojiPickerWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void dialogRequestsRetainTheirOwnBackendAndRequester()
    {
        Backend first;
        Backend second;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        first.emojiRegistry().addCustomEmoji("server_one", dir.filePath("one.png"));
        second.emojiRegistry().addCustomEmoji("server_two", dir.filePath("two.png"));
        QWidget owner;
        QWidget otherOwner;
        QWidget firstRequester(&owner);
        QWidget secondRequester(&otherOwner);
        owner.show();
        otherOwner.show();
        int firstCalls = 0;
        int secondCalls = 0;
        const auto chooser = [] {
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<ChooseEmojiDialog*>(widget))
                    return dialog;
            return static_cast<ChooseEmojiDialog*>(nullptr);
        };
        const auto customPage = [](EmojiPickerWidget* picker) {
            return picker->findChild<QWidget*>(
                QStringLiteral("tab") + QString::number(EmojiCategory::custom));
        };
        const auto hasEmoji = [](QWidget* page, const QString& name) {
            if (!page) return false;
            for (auto* button : page->findChildren<QPushButton*>())
                if (button->property("mattermostEmojiName").toString() == name)
                    return true;
            return false;
        };
        ChooseEmojiDialog::showFor(first, firstRequester,
                                    [&firstCalls](Emoji) { ++firstCalls; });
        auto* dialog = chooser();
        QVERIFY(dialog);
        auto* picker = dialog->findChild<EmojiPickerWidget*>();
        QVERIFY(picker);
        auto* smileys = picker->findChild<QWidget*>("tab0");
        QVERIFY(smileys);
        QVERIFY(hasEmoji(customPage(picker), "server_one"));
        QVERIFY(!hasEmoji(customPage(picker), "server_two"));
        emit picker->emojiChosen({QStringLiteral("eyes"), QStringLiteral("eyes")});
        QCOMPARE(firstCalls, 1);
        ChooseEmojiDialog::showFor(second, secondRequester,
                                    [&secondCalls](Emoji) { ++secondCalls; });
        QCOMPARE(chooser(), dialog);
        QCOMPARE(dialog->findChild<EmojiPickerWidget*>(), picker);
        QCOMPARE(picker->findChild<QWidget*>("tab0"), smileys);
        // Equal-sized custom catalogs must still be updated on account switch.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!hasEmoji(customPage(picker), "server_one"));
        QVERIFY(hasEmoji(customPage(picker), "server_two"));
        emit picker->emojiChosen({QStringLiteral("eyes"), QStringLiteral("eyes")});
        QCOMPARE(firstCalls, 1);
        QCOMPARE(secondCalls, 1);
        auto* temporaryRequester = new QWidget(&otherOwner);
        ChooseEmojiDialog::showFor(second, *temporaryRequester,
                                    [&secondCalls](Emoji) { ++secondCalls; });
        delete temporaryRequester;
        QVERIFY(!dialog->isVisible());
        emit picker->emojiChosen({QStringLiteral("eyes"), QStringLiteral("eyes")});
        QCOMPARE(secondCalls, 1);
    }

    void skinToneModelIsStrict()
    {
        const auto variants =
            EmojiInfo::skinToneVariantsByName(QStringLiteral("+1"));
        QCOMPARE(static_cast<int>(variants.size()),
                 static_cast<int>(EmojiSkinTone::COUNT));
        QCOMPARE(variants.at(EmojiSkinTone::medium).name,
                 QStringLiteral("+1_medium_skin_tone"));
        QCOMPARE(variants.at(EmojiSkinTone::medium).unicodeString,
                 QString::fromUtf8("👍🏽"));

        QVERIFY(EmojiInfo::skinToneVariantsByName(
                    QStringLiteral("eyes")).isEmpty());
        QVERIFY(!EmojiInfo::resolveBuiltInByName(
            QStringLiteral("eyes_medium_skin_tone")));
    }

    void appliesDefaultToneAndOffersLongPressOverride()
    {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
            QSKIP("Qt Windows offscreen does not reliably synthesize popup mouse interaction");
        }
#endif
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
        QApplication::processEvents();

        auto* stack = picker.findChild<QStackedWidget*>();
        QVERIFY(stack);
        // People is the second visible emoji category (Component has no tab).
        stack->setCurrentIndex(1);
        QApplication::processEvents();

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

        thumb->click();
        QVERIFY(chosen);
        QCOMPARE(chosen->name, QStringLiteral("+1_medium_skin_tone"));
        QCOMPARE(chosen->unicodeString, QString::fromUtf8("👍🏽"));

        QScrollArea* peopleScroll = nullptr;
        const auto scrollAreas = picker.findChildren<QScrollArea*>();
        for (QScrollArea* area : scrollAreas) {
            if (area && area->widget()
                && area->widget()->isAncestorOf(thumb)) {
                peopleScroll = area;
                break;
            }
        }
        QVERIFY(peopleScroll);
        peopleScroll->ensureWidgetVisible(thumb, 0, 0);
        QApplication::processEvents();
        QVERIFY(thumb->isVisible());

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
