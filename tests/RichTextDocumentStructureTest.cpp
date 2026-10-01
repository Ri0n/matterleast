#include <QtTest>

#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextTable>

namespace {

QTextTable* firstTable(QTextDocument& document)
{
    QTextCursor cursor(&document);
    cursor.movePosition(QTextCursor::Start);
    if (QTextTable* table = cursor.currentTable()) {
        return table;
    }

    for (QTextFrame::iterator it = document.rootFrame()->begin();
         !it.atEnd(); ++it) {
        if (QTextFrame* frame = it.currentFrame()) {
            if (auto* table = dynamic_cast<QTextTable*>(frame)) {
                return table;
            }
        }
    }
    return nullptr;
}

QString tableMarkdown(int rows, int columns,
                      const std::function<void(QTextTable&)>& populate)
{
    QTextDocument document;
    QTextCursor cursor(&document);
    QTextTable* table = cursor.insertTable(rows, columns);
    Q_ASSERT(table);
    populate(*table);
    return document.toMarkdown(QTextDocument::MarkdownDialectGitHub);
}

} // namespace

class RichTextDocumentStructureTest : public QObject
{
    Q_OBJECT

private slots:
    void tableRoundTripsDimensionsAndCells()
    {
        const QString markdown = tableMarkdown(2, 2, [](QTextTable& table) {
            table.cellAt(0, 0).firstCursorPosition().insertText(QStringLiteral("A"));
            table.cellAt(0, 1).firstCursorPosition().insertText(QStringLiteral("B"));
            table.cellAt(1, 0).firstCursorPosition().insertText(QStringLiteral("C"));
            table.cellAt(1, 1).firstCursorPosition().insertText(QStringLiteral("D"));
        });

        QVERIFY2(markdown.contains(QLatin1Char('|')), qPrintable(markdown));
        for (const QString& value : {QStringLiteral("A"), QStringLiteral("B"),
                                     QStringLiteral("C"), QStringLiteral("D")}) {
            QVERIFY2(markdown.contains(value), qPrintable(markdown));
        }

        QTextDocument parsed;
        parsed.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
        QTextTable* table = firstTable(parsed);
        QVERIFY2(table, qPrintable(markdown));
        QCOMPARE(table->rows(), 2);
        QCOMPARE(table->columns(), 2);
        QCOMPARE(table->cellAt(0, 0).firstCursorPosition().block().text(),
                 QStringLiteral("A"));
        QCOMPARE(table->cellAt(0, 1).firstCursorPosition().block().text(),
                 QStringLiteral("B"));
        QCOMPARE(table->cellAt(1, 0).firstCursorPosition().block().text(),
                 QStringLiteral("C"));
        QCOMPARE(table->cellAt(1, 1).firstCursorPosition().block().text(),
                 QStringLiteral("D"));
    }

    void tableRoundTripsEmptyCellsAndInlineLink()
    {
        const QString markdown = tableMarkdown(2, 2, [](QTextTable& table) {
            QTextCursor linked = table.cellAt(0, 0).firstCursorPosition();
            QTextCharFormat linkFormat;
            linkFormat.setAnchor(true);
            linkFormat.setAnchorHref(QStringLiteral("https://example.test/path"));
            linked.insertText(QStringLiteral("link"), linkFormat);
            table.cellAt(1, 1).firstCursorPosition().insertText(QStringLiteral("tail"));
        });

        QVERIFY2(markdown.contains(QStringLiteral("link")), qPrintable(markdown));
        QVERIFY2(markdown.contains(QStringLiteral("https://example.test/path")),
                 qPrintable(markdown));

        QTextDocument parsed;
        parsed.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
        QTextTable* table = firstTable(parsed);
        QVERIFY2(table, qPrintable(markdown));
        QCOMPARE(table->rows(), 2);
        QCOMPARE(table->columns(), 2);
        QCOMPARE(table->cellAt(1, 1).firstCursorPosition().block().text(),
                 QStringLiteral("tail"));
    }

    void tableMultilineCellRoundTripIsStable()
    {
        const QString markdown = tableMarkdown(2, 2, [](QTextTable& table) {
            QTextCursor cell = table.cellAt(0, 0).firstCursorPosition();
            cell.insertText(QStringLiteral("first"));
            cell.insertText(QString(QChar::LineSeparator));
            cell.insertText(QStringLiteral("second"));
            table.cellAt(0, 1).firstCursorPosition().insertText(QStringLiteral("B"));
        });

        QTextDocument parsed;
        parsed.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
        QTextTable* table = firstTable(parsed);
        QVERIFY2(table, qPrintable(markdown));
        QCOMPARE(table->rows(), 2);
        QCOMPARE(table->columns(), 2);

        const QString text = table->cellAt(0, 0).firstCursorPosition().block().text();
        QVERIFY2(text.contains(QStringLiteral("first")), qPrintable(markdown));
        QVERIFY2(text.contains(QStringLiteral("second")), qPrintable(markdown));
    }
};

QTEST_MAIN(RichTextDocumentStructureTest)
#include "RichTextDocumentStructureTest.moc"
