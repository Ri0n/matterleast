#include <QtTest>

#include <QTabWidget>

#include "SettingsWindow.h"

using namespace Mattermost;

class AiCompileTimeGateTest final : public QObject
{
    Q_OBJECT

private slots:
    void aiSettingsMatchBuildFlag()
    {
        SettingsWindow dialog;
        bool foundAiTab = false;
        for (QTabWidget* tabs : dialog.findChildren<QTabWidget*>()) {
            for (int i = 0; i < tabs->count(); ++i) {
                if (tabs->tabText(i) == QStringLiteral("AI")) {
                    foundAiTab = true;
                }
            }
        }
#ifdef MATTERLEAST_ENABLE_AI
        QVERIFY2(foundAiTab, "AI settings must exist in opt-in builds");
#else
        QVERIFY2(!foundAiTab, "AI settings must not exist in standard builds");
#endif
    }
};

QTEST_MAIN(AiCompileTimeGateTest)
#include "AiCompileTimeGateTest.moc"
