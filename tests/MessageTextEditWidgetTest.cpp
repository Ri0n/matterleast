#include <QtTest>

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QListView>

#include "Settings.h"
#include "chat-area/outgoing-post/MessageTextEditWidget.h"
#include "options/MLOptions.h"

using namespace Mattermost;

class MessageTextEditWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void growsAndShrinksWithExplicitLines()
    {
        MessageTextEditWidget editor;
        editor.resize(320, 40);
        editor.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        editor.setPlainText(QStringLiteral("first"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const int oneLineHeight = editor.height();
        QVERIFY(oneLineHeight > 0);

        editor.setPlainText(QStringLiteral("first\nsecond"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        const int twoLineHeight = editor.height();
        QVERIFY2(twoLineHeight > oneLineHeight,
                 "Adding an explicit second line must increase composer height");

        editor.setPlainText(QStringLiteral("first"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCOMPARE(editor.height(), oneLineHeight);
    }

    void capsGrowthAtMaximumComposerHeight()
    {
        MessageTextEditWidget editor;
        editor.resize(320, 40);
        editor.show();
        QCoreApplication::processEvents();

        QStringList lines;
        for (int i = 0; i < 100; ++i) {
            lines.push_back(QStringLiteral("line %1").arg(i));
        }
        editor.setPlainText(lines.join(QLatin1Char('\n')));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QCOMPARE(editor.height(), 300);
    }

    void submitShortcutFollowsLiveOption()
    {
        auto* sendWithCtrlEnter = MLOptions::instance()->optionObject<bool>(
            COMPOSER_SEND_WITH_CTRL_ENTER,
            COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT);
        const bool previous = sendWithCtrlEnter->value().toBool();

        MessageTextEditWidget editor;
        editor.resize(320, 40);
        editor.show();
        editor.setFocus();
        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);
        QCoreApplication::processEvents();

        sendWithCtrlEnter->setValue(false);

        editor.setPlainText(QStringLiteral("first"));
        editor.moveCursor(QTextCursor::End);
        QTest::keyClick(&editor, Qt::Key_Return);
        QCOMPARE(submitted.count(), 1);
        QCOMPARE(editor.toPlainText(), QStringLiteral("first"));

        submitted.clear();
        editor.setPlainText(QStringLiteral("first"));
        editor.moveCursor(QTextCursor::End);
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(submitted.count(), 0);
        QCOMPARE(editor.toPlainText(), QStringLiteral("first\n"));

        submitted.clear();
        editor.setPlainText(QStringLiteral("first"));
        editor.moveCursor(QTextCursor::End);
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(submitted.count(), 0);
        QVERIFY(editor.toPlainText().startsWith(QStringLiteral("first")));
        QVERIFY(editor.toPlainText().size() > QStringLiteral("first").size());

        sendWithCtrlEnter->setValue(true);

        submitted.clear();
        editor.setPlainText(QStringLiteral("first"));
        editor.moveCursor(QTextCursor::End);
        QTest::keyClick(&editor, Qt::Key_Return);
        QCOMPARE(submitted.count(), 0);
        QCOMPARE(editor.toPlainText(), QStringLiteral("first\n"));

        submitted.clear();
        editor.setPlainText(QStringLiteral("first"));
        editor.moveCursor(QTextCursor::End);
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(submitted.count(), 1);
        QCOMPARE(editor.toPlainText(), QStringLiteral("first"));

        submitted.clear();
        editor.setPlainText(QStringLiteral("first"));
        editor.moveCursor(QTextCursor::End);
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(submitted.count(), 0);
        QVERIFY(editor.toPlainText().startsWith(QStringLiteral("first")));
        QVERIFY(editor.toPlainText().size() > QStringLiteral("first").size());

        sendWithCtrlEnter->setValue(previous);
    }

    void completesAtMentionWithoutReplacingSurroundingMessage()
    {
        QWidget host;
        host.resize(760, 480);

        MessageTextEditWidget editor(&host);
        editor.setGeometry(120, 400, 360, 40);

        InteractiveTextEdit::CompletionRule rule;
        rule.prefix = QStringLiteral("@");
        rule.provider = [] {
            InteractiveTextEdit::CompletionCandidate alice;
            alice.displayText = QStringLiteral("Alice Example");
            alice.insertText = QStringLiteral("alice");
            alice.detailText = QStringLiteral("@alice");
            alice.filterKeys = QStringList {QStringLiteral("Example")};
            return QVector<InteractiveTextEdit::CompletionCandidate> {alice};
        };
        editor.setCompletionRules({std::move(rule)});

        host.show();
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();

        QTest::keyClicks(&editor, QStringLiteral("hello @exa"));
        QCoreApplication::processEvents();
        QVERIFY(!editor.completionPopupVisible());

        QTest::qWait(350);
        QCoreApplication::processEvents();
        QVERIFY(editor.completionPopupVisible());

        auto* overlay =
            host.findChild<QListView*>(QStringLiteral("completionOverlay"));
        QVERIFY(overlay);
        QVERIFY(overlay->isVisible());
        QVERIFY(!overlay->isWindow());
        QCOMPARE(overlay->window(), static_cast<QWidget*>(&host));
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(&editor));

        const QRect caret = editor.cursorRect();
        const QPoint caretTop =
            editor.viewport()->mapTo(&host, caret.topLeft());
        const QPoint caretBottom =
            editor.viewport()->mapTo(&host, caret.bottomLeft());
        const QRect overlayRect = overlay->geometry();
        QVERIFY2(overlayRect.bottom() <= caretTop.y()
                     || overlayRect.top() >= caretBottom.y(),
                 "Completion overlay must stay adjacent to the caret, not cover it");

        QTest::keyClick(&editor, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(editor.toPlainText(), QStringLiteral("hello @alice "));
    }

    void completionStaysHiddenWhileBackspaceRepeats()
    {
        QWidget host;
        host.resize(760, 480);

        MessageTextEditWidget editor(&host);
        editor.setGeometry(120, 400, 360, 40);

        InteractiveTextEdit::CompletionRule rule;
        rule.prefix = QStringLiteral("@");
        rule.provider = [] {
            QVector<InteractiveTextEdit::CompletionCandidate> result;
            for (const QString& name : {
                     QStringLiteral("alice"),
                     QStringLiteral("alex"),
                     QStringLiteral("alfred")}) {
                InteractiveTextEdit::CompletionCandidate candidate;
                candidate.displayText = name;
                candidate.insertText = name;
                candidate.detailText = QStringLiteral("@") + name;
                result.push_back(std::move(candidate));
            }
            return result;
        };
        editor.setCompletionRules({std::move(rule)});

        host.show();
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();

        QTest::keyClicks(&editor, QStringLiteral("@alice"));
        QTest::qWait(350);
        QCoreApplication::processEvents();
        QVERIFY(editor.completionPopupVisible());

        auto* overlay =
            host.findChild<QListView*>(QStringLiteral("completionOverlay"));
        QVERIFY(overlay);
        QVERIFY(!overlay->isWindow());
        QVERIFY(QApplication::activePopupWidget() == nullptr);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(&editor));

        auto sendKey = [](QWidget* target,
                          QEvent::Type type,
                          bool autoRepeat) {
            QKeyEvent event(
                type, Qt::Key_Backspace, Qt::NoModifier,
                QString(), autoRepeat, 1);
            QCoreApplication::sendEvent(target, &event);
        };

        // One physical key hold: initial press, repeated auto-repeat presses,
        // then a single final release. Every event is routed to whichever
        // widget actually has focus at that moment.
        QWidget* target = QApplication::focusWidget();
        QVERIFY(target);
        sendKey(target, QEvent::KeyPress, false);
        QCoreApplication::processEvents();
        QCOMPARE(editor.toPlainText(), QStringLiteral("@alic"));
        QVERIFY(!editor.completionPopupVisible());

        for (int i = 0; i < 3; ++i) {
            QTest::qWait(75);
            target = QApplication::focusWidget();
            QCOMPARE(target, static_cast<QWidget*>(&editor));
            sendKey(target, QEvent::KeyPress, true);
            QCoreApplication::processEvents();
            QVERIFY(!editor.completionPopupVisible());
        }

        QCOMPARE(editor.toPlainText(), QStringLiteral("@a"));

        target = QApplication::focusWidget();
        QCOMPARE(target, static_cast<QWidget*>(&editor));
        sendKey(target, QEvent::KeyRelease, false);

        QTest::qWait(350);
        QCoreApplication::processEvents();
        QVERIFY(editor.completionPopupVisible());
        QVERIFY(!overlay->isWindow());
        QVERIFY(QApplication::activePopupWidget() == nullptr);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(&editor));
    }

    void completionQueryChangesAreDeduplicatedAndCancelled()
    {
        MessageTextEditWidget editor;
        QStringList queries;

        InteractiveTextEdit::CompletionRule rule;
        rule.prefix = QStringLiteral("@");
        rule.provider = [] {
            InteractiveTextEdit::CompletionCandidate alice;
            alice.displayText = QStringLiteral("Alice Example");
            alice.insertText = QStringLiteral("alice");
            alice.detailText = QStringLiteral("@alice");
            return QVector<InteractiveTextEdit::CompletionCandidate> {alice};
        };
        rule.queryChanged = [&queries](const QString& query) {
            queries.push_back(query);
        };
        editor.setCompletionRules({std::move(rule)});
        editor.resize(360, 40);
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();

        QTest::keyClicks(&editor, QStringLiteral("@al"));
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QCOMPARE(queries.count(QStringLiteral("al")), 1);
        QCOMPARE(queries.constLast(), QStringLiteral("al"));

        // An asynchronous provider refresh for the same query must only rebuild
        // the popup; it must not look like another user query and retrigger I/O.
        editor.refreshCompletions();
        editor.refreshCompletions();
        QCoreApplication::processEvents();
        QCOMPARE(queries.count(QStringLiteral("al")), 1);

        QTest::keyClick(&editor, Qt::Key_Space);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QVERIFY(!queries.isEmpty());
        QCOMPARE(queries.constLast(), QString());
    }
};

QTEST_MAIN(MessageTextEditWidgetTest)

#include "MessageTextEditWidgetTest.moc"
