#include <QtTest>

#include <QWidget>

#include "SettingsWindow.h"

using namespace Mattermost;

class AiCompileTimeGateTest final : public QObject
{
    Q_OBJECT

private slots:
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
