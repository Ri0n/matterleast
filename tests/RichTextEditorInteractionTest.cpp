#include <QtTest>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextList>
#include <QTextTable>

#include "Settings.h"
#include "chat-area/CodeBlockSupport.h"
#include "chat-area/outgoing-post/MessageTextEditWidget.h"
#include "chat-area/outgoing-post/RichTextEditorCommands.h"
#include "chat-area/post/MessageContentWidget.h"
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

void selectWholeDocument(MessageTextEditWidget& editor)
{
    QTextCursor cursor(editor.document());
    cursor.setPosition(0);
    cursor.setPosition(editor.document()->characterCount() - 1,
                       QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
}

int listIndent(const QTextBlock& block)
{
    return block.textList() ? block.textList()->format().indent() : 0;
}

QTextTable* firstTable(QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        if (QTextTable* table = cursor.currentTable()) {
            return table;
        }
    }
    return nullptr;
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

    void renderedMessageListRemainsStructural()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("- first\n- second"));
        widget.resize(480, 120);
        widget.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        auto* richText = widget.findChild<QTextBrowser*>(
            QStringLiteral("messageRichText"));
        QVERIFY(richText);

        const QTextBlock first = richText->document()->firstBlock();
        const QTextBlock second = first.next();
        QVERIFY(first.isValid());
        QVERIFY(second.isValid());
        QVERIFY2(first.textList(), qPrintable(richText->document()->toHtml()));
        QVERIFY2(second.textList(), qPrintable(richText->document()->toHtml()));
        QCOMPARE(second.textList(), first.textList());
    }

    void listToolbarConvertsAndTogglesSelection()
    {
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("first\n\nsecond"));

        selectWholeDocument(editor);
        editor.toggleBulletList();
        QTextBlock first = editor.document()->firstBlock();
        QTextBlock second = first.next();
        QVERIFY(first.textList());
        QVERIFY(second.isValid());
        QVERIFY(second.textList());
        QCOMPARE(first.textList()->format().style(), QTextListFormat::ListDisc);
        QCOMPARE(second.textList()->format().style(), QTextListFormat::ListDisc);

        selectWholeDocument(editor);
        editor.toggleNumberedList();
        first = editor.document()->firstBlock();
        second = first.next();
        QVERIFY(first.textList());
        QVERIFY(second.textList());
        QCOMPARE(first.textList()->format().style(), QTextListFormat::ListDecimal);
        QCOMPARE(second.textList()->format().style(), QTextListFormat::ListDecimal);

        selectWholeDocument(editor);
        editor.toggleNumberedList();
        QVERIFY(editor.document()->firstBlock().textList() == nullptr);
        QVERIFY(editor.document()->firstBlock().next().textList() == nullptr);
    }

    void listSplitIsSingleUndoStep()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- first"));

        placeCursorAtBlockEnd(editor, editor.document()->firstBlock());
        QTest::keyClick(&editor, Qt::Key_Return);
        QCOMPARE(editor.document()->blockCount(), 2);
        QVERIFY(editor.document()->firstBlock().next().textList());

        editor.undo();
        QCOMPARE(editor.document()->blockCount(), 1);
        QCOMPARE(editor.document()->firstBlock().text(), QStringLiteral("first"));
        QVERIFY(editor.document()->firstBlock().textList());
    }

    void listExitIsSingleUndoStep()
    {
        SendOptionGuard option(false);
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("- first"));

        placeCursorAtBlockEnd(editor, editor.document()->firstBlock());
        QTest::keyClick(&editor, Qt::Key_Return);
        QVERIFY(editor.textCursor().block().textList());
        QTest::keyClick(&editor, Qt::Key_Return);
        QVERIFY(editor.textCursor().block().textList() == nullptr);

        editor.undo();
        const QTextBlock second = editor.document()->firstBlock().next();
        QVERIFY(second.isValid());
        QVERIFY(second.textList());
        QVERIFY(second.text().isEmpty());
    }

    void listIndentWidthDoesNotChangeMarkdown()
    {
        const QString source = QStringLiteral("- parent\n  - child");

        QTextDocument document;
        document.setMarkdown(source, QTextDocument::MarkdownDialectGitHub);
        const QString before = document.toMarkdown(
            QTextDocument::MarkdownDialectGitHub);
        QVERIFY(document.indentWidth() > 20.0);

        document.setIndentWidth(20.0);
        QCOMPARE(document.indentWidth(), qreal(20.0));
        QCOMPARE(document.toMarkdown(QTextDocument::MarkdownDialectGitHub), before);

        MessageTextEditWidget editor;
        prepareEditor(editor, source);
        QCOMPARE(editor.document()->indentWidth(), qreal(20.0));
        QCOMPARE(editor.markdownText(), source);
    }

    void structuralPresentationPreservesCanonicalMarkdown()
    {
        const QString source = QStringLiteral(
            "> quoted text\n\n"
            "```cpp\n"
            "int answer = 42;\n"
            "```");

        MessageTextEditWidget editor;
        prepareEditor(editor, source);
        QCoreApplication::processEvents();

        // Quote/code styling is presentation-only. Showing the rich editor must
        // not dirty the source snapshot or serialize the document back through
        // QTextDocument merely to obtain the target visual appearance.
        QCOMPARE(editor.markdownText(), source);

        bool hasMutedQuote = false;
        bool hasCodeBackground = false;
        const QColor ordinaryText = editor.palette().color(QPalette::Text);
        for (const QTextEdit::ExtraSelection& selection : editor.extraSelections()) {
            const QColor foreground = selection.format.foreground().color();
            const QColor background = selection.format.background().color();
            if (foreground.isValid()
                && foreground.alpha() < ordinaryText.alpha()) {
                hasMutedQuote = true;
            }
            if (background == QColor(39, 40, 34)) {
                hasCodeBackground = true;
            }
        }
        QVERIFY(hasMutedQuote);
        QVERIFY(hasCodeBackground);

        QWidget* overlay = editor.findChild<QWidget*>(
            QStringLiteral("_matterleast_rich_block_decoration_overlay"));
        QVERIFY(overlay);

        const QTextBlock quote = editor.document()->firstBlock();
        QTextCursor quoteCursor(quote);
        const int quoteY = editor.cursorRect(quoteCursor).center().y();
        const QImage overlayImage = overlay->grab().toImage();
        QVERIFY(!overlayImage.isNull());
        QVERIFY(quoteY >= 0 && quoteY < overlayImage.height());
        QCOMPARE(overlayImage.pixelColor(2, quoteY),
                 editor.palette().color(QPalette::Mid));
    }

    void toolbarCodeBlockGetsPresentation()
    {
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("code"));
        selectWholeDocument(editor);
        editor.toggleCodeBlock();
        QCoreApplication::processEvents();

        QVERIFY(isStructuralCodeBlock(editor.document()->firstBlock()));
        bool hasCodeBackground = false;
        for (const QTextEdit::ExtraSelection& selection : editor.extraSelections()) {
            if (selection.format.background().color() == QColor(39, 40, 34)) {
                hasCodeBackground = true;
                break;
            }
        }
        QVERIFY(hasCodeBackground);
        QVERIFY2(editor.markdownText().contains(QStringLiteral("```")),
                 qPrintable(editor.markdownText()));
    }

    void codeLanguageAppliesToWholeBlockAndSerializes()
    {
        MessageTextEditWidget editor;
        prepareEditor(editor, QStringLiteral("```\nfirst\nsecond\n```"));
        editor.setTextCursor(QTextCursor(editor.document()->firstBlock()));

        QVERIFY(RichTextEditorCommands::setCodeBlockLanguage(
            editor, QStringLiteral("c++")));
        QCOMPARE(RichTextEditorCommands::codeBlockLanguageAt(editor),
                 QStringLiteral("cpp"));

        bool sawCode = false;
        for (QTextBlock block = editor.document()->begin();
             block.isValid(); block = block.next()) {
            if (!isStructuralCodeBlock(block)) {
                continue;
            }
            sawCode = true;
            QCOMPARE(codeBlockLanguage(block), QStringLiteral("cpp"));
        }
        QVERIFY(sawCode);

        const QString markdown = editor.markdownText();
        QVERIFY2(markdown.contains(QStringLiteral("```cpp")), qPrintable(markdown));

        QVERIFY(RichTextEditorCommands::setCodeBlockLanguage(editor, QString()));
        QVERIFY(RichTextEditorCommands::codeBlockLanguageAt(editor).isEmpty());
        QVERIFY(isStructuralCodeBlock(editor.textCursor().block()));
    }

    void nativeQtTableMarkdownIsNotRoundTripSafe()
    {
        QTextDocument document;
        QTextCursor cursor(&document);
        QTextTable* table = cursor.insertTable(2, 2);
        QVERIFY(table);
        table->cellAt(0, 0).firstCursorPosition().insertText(QStringLiteral("A"));
        table->cellAt(0, 1).firstCursorPosition().insertText(QStringLiteral("B"));
        table->cellAt(1, 1).firstCursorPosition().insertText(QStringLiteral("D"));

        const QString markdown = document.toMarkdown(
            QTextDocument::MarkdownDialectGitHub);
        QVERIFY2(markdown.contains(QLatin1Char('|')), qPrintable(markdown));
        QVERIFY2(markdown.contains(QStringLiteral("A")), qPrintable(markdown));
        QVERIFY2(markdown.contains(QStringLiteral("D")), qPrintable(markdown));

        QTextDocument parsed;
        parsed.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
        QTextTable* parsedTable = firstTable(parsed);

        // Qt 5.15, 6.4 and 6.11 currently serialize this table with a delimiter
        // row such as "|-|-|". GFM requires at least three '-' characters per
        // delimiter cell, and Qt's own parser consequently does not reconstruct
        // a QTextTable. Keep this as an explicit serialization gate: an XPASS
        // on a future Qt version tells us to reconsider whether the owned
        // MatterLeast table serializer is still necessary.
        QEXPECT_FAIL(
            "",
            "Native Qt GFM table Markdown is not round-trip safe; use the MatterLeast table serializer",
            Abort);
        QVERIFY2(parsedTable, qPrintable(markdown));
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
        QVERIFY(isStructuralCodeBlock(code));
        placeCursorAtBlockEnd(editor, code);
        QTest::keyClick(&editor, Qt::Key_Return);

        QCOMPARE(submitted.count(), 0);
        QTextBlock empty = editor.textCursor().block();
        QVERIFY(isStructuralCodeBlock(empty));
        QVERIFY(empty.text().isEmpty());

        QTest::keyClick(&editor, Qt::Key_Backspace);
        QVERIFY(!isStructuralCodeBlock(editor.textCursor().block()));
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