#include <QtTest>

#include <QJsonObject>
#include <QMenu>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "channel-tree/ChannelTree.h"
#include "options/MLOptions.h"
#include "ui/TeamSelectorLabel.h"

using namespace Mattermost;

class TeamToolbarTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void teamButtonsSwitchAndPersistThroughExistingController()
    {
        auto* settings = MLOptions::instance();
        const QString key = QStringLiteral("sidebar/active_team_id");
        const QString previous = settings->value<QString>(key);
        struct Restore {
            MLOptions* settings;
            QString key, previous;
            ~Restore() { settings->setValue(key, previous); }
        } restore {settings, key, previous};
        settings->setValue(key, QStringLiteral("team-alpha"));

        Backend backend;
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* selector = new TeamSelectorLabel(&window);
        selector->setObjectName(QStringLiteral("teamSelectorLabel"));
        layout->addWidget(selector);
        auto* tree = new ChannelTree(&window);
        tree->setObjectName(QStringLiteral("channelList"));
        auto* toolbar = selector->createTeamToolbar(&window);
        layout->addWidget(toolbar);
        layout->addWidget(tree);
        tree->populateSidebars(backend);

        auto addRoot = [&](const QString& id, const QString& display) {
            BackendTeam* team = backend.getStorage().addTeam(QJsonObject {
                {"id", id}, {"name", display}, {"display_name", display}
            });
            QVERIFY(team);
            auto* root = new QTreeWidgetItem(tree);
            root->setText(0, display);
            root->setData(0, ChannelTree::ItemKindRole,
                          ChannelTree::TeamItemKind);
            root->setData(0, ChannelTree::ItemTeamIdRole, id);
        };
        addRoot(QStringLiteral("team-alpha"), QStringLiteral("Alpha team"));
        addRoot(QStringLiteral("team-beta"), QStringLiteral("Beta team"));
        window.resize(320, 420);
        window.show();
        QTRY_COMPARE(toolbar->findChildren<QToolButton*>(
            QStringLiteral("teamToolbarTeamButton")).size(), 2);
        QCOMPARE(selector->activeTeamId(), QStringLiteral("team-alpha"));

        QToolButton* beta = nullptr;
        for (QToolButton* button : toolbar->findChildren<QToolButton*>(
                 QStringLiteral("teamToolbarTeamButton"))) {
            if (button->property("teamId").toString() == QLatin1String("team-beta")) {
                beta = button;
            }
        }
        QVERIFY(beta);
        QVERIFY(!beta->isChecked());
        QVERIFY(toolbar->findChild<QToolButton*>(
            QStringLiteral("teamToolbarAdd")));
        auto* actions = toolbar->findChild<QToolButton*>(
            QStringLiteral("teamToolbarActions"));
        QVERIFY(actions);
        QVERIFY(actions->menu());
        QVERIFY(!actions->menu()->actions().isEmpty());

        QTest::mouseClick(beta, Qt::LeftButton);
        QCOMPARE(selector->activeTeamId(), QStringLiteral("team-beta"));
        QCOMPARE(settings->value<QString>(key), QStringLiteral("team-beta"));
        QCOMPARE(tree->rootIndex().data(ChannelTree::ItemTeamIdRole).toString(),
                 QStringLiteral("team-beta"));
        QTRY_COMPARE(toolbar->findChildren<QToolButton*>(
            QStringLiteral("teamToolbarTeamButton")).size(), 2);
        bool betaChecked = false;
        for (QToolButton* button : toolbar->findChildren<QToolButton*>(
                 QStringLiteral("teamToolbarTeamButton"))) {
            if (button->property("teamId").toString() == QLatin1String("team-beta")) {
                betaChecked = button->isChecked();
            }
        }
        QVERIFY(betaChecked);
    }
};

QTEST_MAIN(TeamToolbarTest)
#include "TeamToolbarTest.moc"
