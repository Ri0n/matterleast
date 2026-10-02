#include <QtTest>

#include <QTextBrowser>
#include <QTextDocument>

#include "chat-area/QuotedPostPreview.h"

using namespace Mattermost;

class QuotedPostPreviewTest : public QObject
{
    Q_OBJECT

private slots:
    void editingPreviewPreservesQuotedLineBreaks()
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        QuotedPostPreview preview(nullptr, 8);
        preview.setPreview(
            QStringLiteral("Editing message"),
            QStringLiteral("> 1\n> 2\n> 3\n\n4\n5\n6"));

        auto* browser = preview.findChild<QTextBrowser*>();
        QVERIFY(browser != nullptr);

        const QString plain = browser->document()->toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("1\n2\n3")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("4\n5\n6")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("1 2 3")), qPrintable(plain));
#else
        QSKIP("Direct Markdown document rendering requires Qt 5.14 or newer");
#endif
    }
};

QTEST_MAIN(QuotedPostPreviewTest)

#include "QuotedPostPreviewTest.moc"
