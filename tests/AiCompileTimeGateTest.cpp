#include <QtTest>

#include <QWidget>

#include "SettingsWindow.h"
#ifdef MATTERLEAST_ENABLE_AI
#include "ai/AiThreadMcpServer.h"
#include "backend/Backend.h"
#endif

using namespace Mattermost;

class AiCompileTimeGateTest final : public QObject
{
    Q_OBJECT

private slots:
    void aiMcpServerIsScopedToItsBackend()
    {
#ifdef MATTERLEAST_ENABLE_AI
        Backend firstBackend;
        Backend secondBackend;
        auto& first = AiThreadMcpServer::instance(firstBackend);
        auto& second = AiThreadMcpServer::instance(secondBackend);
        QVERIFY(&first != &second);
        QCOMPARE(&first, &AiThreadMcpServer::instance(firstBackend));
        QCOMPARE(&second, &AiThreadMcpServer::instance(secondBackend));
        QCOMPARE(first.parent(), static_cast<QObject*>(&firstBackend));
        QCOMPARE(second.parent(), static_cast<QObject*>(&secondBackend));
        QVERIFY(!first.listening());
        QVERIFY(!second.listening());
#else
        QSKIP("AI MCP is intentionally not part of this build");
#endif
    }

    void aiSettingsMatchBuildFlag()
    {
        SettingsWindow dialog;
        const bool foundAiTab =
            dialog.findChild<QWidget*>(QStringLiteral("aiSettingsTab")) != nullptr;
#ifdef MATTERLEAST_ENABLE_AI
        QVERIFY2(foundAiTab, "AI settings must exist in opt-in builds");
#else
        QVERIFY2(!foundAiTab, "AI settings must not exist in standard builds");
#endif
    }
};

QTEST_MAIN(AiCompileTimeGateTest)
#include "AiCompileTimeGateTest.moc"
