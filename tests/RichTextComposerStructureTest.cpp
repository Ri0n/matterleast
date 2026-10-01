#include <QtTest>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextList>

#include "chat-area/outgoing-post/MessageTextEditWidget.h"

using namespace Mattermost;

class RichTextComposerStructureTest : public QObject
{
    Q_OBJECT

private slots:
    void listStartedInEmptyComposerSerializesAsMarkdownList()
    {
        MessageTextEditWidget editor;
        editor.setRichTextEditing(true);
        editor.setMarkdownText(QString());
        editor.resize(360, 80);
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();

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

    void listEnterDoesNotSubmitInDefaultSendMode()
    {
        MessageTextEditWidget editor;
        editor.setRichTextEditing(true);
        editor.setMarkdownText(QString());
        editor.resize(360, 80);
        editor.show();
        editor.setFocus();
        QCoreApplication::processEvents();

        QSignalSpy submitted(&editor, &MessageTextEditWidget::enterPressed);
        editor.toggleBulletList();
        QTest::keyClicks(&editor, QStringLiteral("item"));
        QTest::keyClick(&editor, Qt::Key_Return);

        QCOMPARE(submitted.count(), 0);
        QVERIFY(editor.textCursor().block().textList());
    }
};

QTEST_MAIN(RichTextComposerStructureTest)
#include "RichTextComposerStructureTest.moc"
