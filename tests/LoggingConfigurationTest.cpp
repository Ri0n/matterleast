#include <QDebug>
#include <QRegularExpression>
#include <QTest>

#include "diagnostics/LoggingConfiguration.h"

using namespace Mattermost;

class LoggingConfigurationTest : public QObject
{
    Q_OBJECT

private slots:
    void allQtCategoriesGetMonotonicMillisecondTimestamps()
    {
        // QT_MESSAGE_PATTERN is an intentional user override. Qt gives it
        // precedence over qSetMessagePattern, so do not assert our default
        // format when the surrounding environment selected another one.
        if (!qgetenv("QT_MESSAGE_PATTERN").isEmpty()) {
            QSKIP("QT_MESSAGE_PATTERN explicitly overrides the default");
        }

        configureApplicationLogging();

        const QMessageLogContext context(
            __FILE__, __LINE__, Q_FUNC_INFO, "mattermost.logging.test");
        const auto format = [&context] {
            return qFormatLogMessage(
                QtInfoMsg, context, QStringLiteral("sample event"));
        };
        const QRegularExpression pattern(QStringLiteral(
            R"(^\[\s*(\d+)\.(\d{3})s\] mattermost\.logging\.test: sample event$)"));
        const auto parseMilliseconds = [&pattern](const QString& line) -> qint64 {
            const QRegularExpressionMatch match = pattern.match(line);
            if (!match.hasMatch()) return -1;
            return match.captured(1).toLongLong() * 1000
                + match.captured(2).toLongLong();
        };

        const QString first = format();
        const qint64 firstMs = parseMilliseconds(first);
        QVERIFY2(firstMs >= 0, qPrintable(first));

        QTest::qWait(10);
        const QString second = format();
        const qint64 secondMs = parseMilliseconds(second);
        QVERIFY2(secondMs >= firstMs + 1, qPrintable(second));

        // Global configuration must apply to ordinary Qt categories too,
        // not just the application-specific "mattermost.*" categories.
        const QMessageLogContext qtContext(
            __FILE__, __LINE__, Q_FUNC_INFO, "qt.network");
        const QString qtLine = qFormatLogMessage(
            QtWarningMsg, qtContext, QStringLiteral("network event"));
        QVERIFY(qtLine.contains(QStringLiteral("qt.network: network event")));
        QVERIFY(qtLine.startsWith(QLatin1Char('[')));
    }
};

QTEST_APPLESS_MAIN(LoggingConfigurationTest)

#include "LoggingConfigurationTest.moc"
