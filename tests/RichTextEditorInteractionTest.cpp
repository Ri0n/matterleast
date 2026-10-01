#include <QtTest>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextList>

#include "Settings.h"
#include "chat-area/outgoing-post/MessageTextEditWidget.h"
#include "chat-area/outgoing-post/RichTextEditorCommands.h"
#include "options/MLOptions.h"

using namespace Mattermost;

namespace {

class SendOptionGuard
{
public:
    explicit SendOptionGuard(bool value)
        : option_(MLOptions::instance()->optionObject<bool>(
              COMPOSER_SEND_WITH_CTRL_ENTER,
              COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT))
        , previous_(option_->value().toBool())
    {
        option_->setValue(value);
    }

    ~SendOptionGuard()
    {
        option_->setValue(previous_);
    }

private:
    MLOptionObject* option_ = nullptr;
    bool previous_ = false;
};

void prepareEditor(MessageTextEditWidget& editor, const QString& markdown)
{
    editor.setRichTextEditing(true);
    editor.setMarkdownText(markdown);
    editor.resize(480, 80);
    editor.show();
    editor.setFocus();
    QCoreApplication::processEvents();
}

void placeCursorAtBlockEnd(MessageTextEditWidget& editor, QTextBlock block)
{
    QTextCursor cursor(block);
    cursor.movePosition(QTextCursor::EndOfBlock);
    editor.setTextCursor(cursor);
}

int listIndent(const QTextBlock& block)
{
    return block.textList() ? block.textList()->format().indent() : 0;
}

} // namespace

class RichTextEditorInteractionTest : public QObject
{
    Q_OBJECT

private slots:
    void listEnterSplitsBeforeSubmit()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- first"));
        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);

        QTextBlock first = editor.document()->firstBlock();
        QVERIFY(first.textList());
        placeCursorAtBlockEnd(editor, first);
        QTest::keyClick(&editor, Qt::Key_Return);

        QCOMPARE(submitted.count(), 0);
        first = editor.document()->firstBlock();
        const QTextBlock second = first.next();
        QVERIFY(first.textList());
        QVERIFY(second.isValid());
        QVERIFY(second.textList());
        QCOMPARE(listIndent(second), listIndent(first));
        QVERIFY(second.text().isEmpty());
    }

    void listStartedInEmptyComposerSerializesAsMarkdownList()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QString());
        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);

        editor.toggleBulletList();
        QVERIFY(editor.textCursor().block().textList());

        QTest::keyClicks(&editor, QStringLiteral("first"));
        QTest::keyClick(&editor, Qt::Key_Return);
        QCOMPARE(submitted.count(), 0);
        QVERIFY(editor.textCursor().block().textList());
        QTest::keyClicks(&editor, QStringLiteral("second"));

        const QString markdown = editor.markdownText();
        QVERIFY2(markdown.contains(QStringLiteral("- first")), qPrintable(markdown));
        QVERIFY2(markdown.contains(QStringLiteral("- second")), qPrintable(markdown));
    }

    void listIndentWidthDoesNotChangeMarkdown()
    {
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- parent\n  - child"));

        const QString before = editor.document()->toMarkdown(
            QTextDocument::MarkdownDialectGitHub);
        const qreal originalIndentWidth = editor.document()->indentWidth();
        QVERIFY(originalIndentWidth > 20.0);

        editor.document()->setIndentWidth(20.0);
        QCOMPARE(editor.document()->indentWidth(), 20.0);

        const QString after = editor.document()->toMarkdown(
            QTextDocument::MarkdownDialectGitHub);
        QCOMPARE(after, before);
    }

    void emptyTopLevelListItemEnterLeavesList()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- first"));
        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);

        placeCursorAtBlockEnd(editor, editor.document()->firstBlock());
        QTest::keyClick(&editor, Qt::Key_Return);
        QVERIFY(editor.textCursor().block().textList());
        QTest::keyClick(&editor, Qt::Key_Return);

        QCOMPARE(submitted.count(), 0);
        QVERIFY(editor.document()->firstBlock().textList());
        QVERIFY(editor.textCursor().block().textList() == nullptr);
    }

    void emptyNestedListItemEnterOutdents()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- parent\n  - child"));

        QTextBlock nested = editor.document()->firstBlock().next();
        QVERIFY(nested.isValid());
        QVERIFY(nested.textList());
        QVERIFY(listIndent(nested) > 1);

        placeCursorAtBlockEnd(editor, nested);
        QTest::keyClick(&editor, Qt::Key_Return);
        QTextBlock empty = editor.textCursor().block();
        QVERIFY(empty.textList());
        const int nestedIndent = listIndent(empty);
        QTest::keyClick(&editor, Qt::Key_Return);

        QVERIFY(editor.textCursor().block().textList());
        QCOMPARE(listIndent(editor.textCursor().block()), nestedIndent - 1);
    }

    void listTabAndBacktabChangeIndent()
    {
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- first\n- second"));

        QTextBlock second = editor.document()->firstBlock().next();
        QVERIFY(second.textList());
        const int initialIndent = listIndent(second);
        placeCursorAtBlockEnd(editor, second);

        QTest::keyClick(&editor, Qt::Key_Tab);
        QCOMPARE(listIndent(editor.textCursor().block()), initialIndent + 1);

        QTest::keyClick(&editor, Qt::Key_Backtab);
        QCOMPARE(listIndent(editor.textCursor().block()), initialIndent);
    }

    void quoteNeedsEmptySecondEnterToExit()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("> quote"));
        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);

        QTextBlock quote = editor.document()->firstBlock();
        QVERIFY(quote.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0);
        placeCursorAtBlockEnd(editor, quote);

        QTest::keyClick(&editor, Qt::Key_Return);
        QTextBlock empty = editor.textCursor().block();
        QCOMPARE(submitted.count(), 0);
        QVERIFY(empty.text().isEmpty());
        QVERIFY(empty.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0);

        QTest::keyClick(&editor, Qt::Key_Return);
        QCOMPARE(submitted.count(), 0);
        QCOMPARE(editor.textCursor().block().blockFormat().intProperty(
                     QTextFormat::BlockQuoteLevel),
                 0);
    }

    void shiftEnterNeverLeavesQuote()
    {
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("> quote"));
        placeCursorAtBlockEnd(editor, editor.document()->firstBlock());

        QTest::keyClick(&editor, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(editor.document()->blockCount(), 1);
        QVERIFY(editor.textCursor().block().blockFormat().intProperty(
                    QTextFormat::BlockQuoteLevel) > 0);
    }

    void codeEnterDoesNotSubmitAndEmptyBackspaceExits()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("```\ncode\n```"));
        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);

        QTextBlock code = editor.document()->firstBlock();
        QVERIFY(code.blockFormat().hasProperty(QTextFormat::BlockCodeFence));
        placeCursorAtBlockEnd(editor, code);
        QTest::keyClick(&editor, Qt::Key_Return);

        QCOMPARE(submitted.count(), 0);
        QTextBlock empty = editor.textCursor().block();
        QVERIFY(empty.blockFormat().hasProperty(QTextFormat::BlockCodeFence));
        QVERIFY(empty.text().isEmpty());

        QTest::keyClick(&editor, Qt::Key_Backspace);
        QVERIFY(!editor.textCursor().block().blockFormat().hasProperty(
            QTextFormat::BlockCodeFence));
    }

    void configuredCtrlEnterStillSubmitsInsideList()
    {
        SendOptionGuard option(true);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- first"));
        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);

        placeCursorAtBlockEnd(editor, editor.document()->firstBlock());
        QTest::keyClick(&editor, Qt::Key_Return, Qt::ControlModifier);

        QCOMPARE(submitted.count(), 1);
        QVERIFY(editor.document()->firstBlock().textList());
        QCOMPARE(editor.document()->blockCount(), 1);
    }

    void mixedInlineSelectionConvergesThenTogglesOff()
    {
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("**bold** plain"));

        QTextCursor selection(editor.document());
        selection.setPosition(0);
        selection.setPosition(editor.document()->characterCount() - 1,
                              QTextCursor::KeepAnchor);
        editor.setTextCursor(selection);

        QVERIFY(RichTextEditorCommands::toggleInline(
            editor, RichTextEditorCommands::InlineStyle::Bold));
        QString markdown = editor.document()->toMarkdown(
            QTextDocument::MarkdownDialectGitHub);
        QVERIFY2(markdown.contains(QStringLiteral("**bold plain**")),
                 qPrintable(markdown));

        selection.setPosition(0);
        selection.setPosition(editor.document()->characterCount() - 1,
                              QTextCursor::KeepAnchor);
        editor.setTextCursor(selection);
        QVERIFY(RichTextEditorCommands::toggleInline(
            editor, RichTextEditorCommands::InlineStyle::Bold));
        markdown = editor.document()->toMarkdown(
            QTextDocument::MarkdownDialectGitHub);
        QVERIFY2(!markdown.contains(QStringLiteral("**")), qPrintable(markdown));
    }
};

QTEST_MAIN(RichTextEditorInteractionTest)
#include "RichTextEditorInteractionTest.moc"
