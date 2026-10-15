#include <QtTest>

#include <QHBoxLayout>
#include <QJsonObject>
#include <QMenu>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "channel-tree/ChannelTree.h"
#include "channel-tree/team-item/GroupTeamItem.h"
#include "options/MLOptions.h"
#include "ui/TeamSelectorLabel.h"

using namespace Mattermost;

class TeamToolbarTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        // The production application sets identity before MLOptions creates
        // The Windows native settings store requires an organization name.
        QCoreApplication::setOrganizationName(QStringLiteral("MatterLeast-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("TeamToolbarTest"));
    }

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
        QCOMPARE(settings->value<QString>(key), QStringLiteral("team-alpha"));

        Backend backend;
        // SidebarService asynchronously reads current-user preferences.
        // A bare Backend has no login user, unlike a real MainWindow session.
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("test-user")},
            {QStringLiteral("username"), QStringLiteral("test-user")}
        }, true);
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* selector = new TeamSelectorLabel(&window);
        selector->setObjectName(QStringLiteral("teamSelectorLabel"));
        layout->addWidget(selector);
        auto* tree = new ChannelTree(&window);
        tree->setObjectName(QStringLiteral("channelList"));
        auto* unreadOnly = new QToolButton(&window);
        unreadOnly->setCheckable(true);
        unreadOnly->setObjectName(QStringLiteral("channelsUnreadFilterButton"));
        auto* toolbar = selector->createTeamToolbar(&window, unreadOnly);
        layout->addWidget(toolbar);
        QCOMPARE(unreadOnly->parentWidget(), toolbar);
        auto* toolbarLayout = qobject_cast<QHBoxLayout*>(toolbar->layout());
        QVERIFY(toolbarLayout);
        QCOMPARE(toolbarLayout->itemAt(0)->widget(), static_cast<QWidget*>(unreadOnly));
        layout->addWidget(tree);
        tree->populateSidebars(backend);

        auto addRoot = [&](const QString& id, const QString& display) {
            BackendTeam* team = backend.getStorage().addTeam(QJsonObject {
                {"id", id}, {"name", display}, {"display_name", display}
            });
            QVERIFY(team);
            // The real tree contains TeamItem-derived roots; generic
            // QTreeWidgetItem stubs violate its navigation assumptions.
            auto* root = new GroupTeamItem(*tree, backend, display, id);
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
        // Teams remain left-aligned directly after the unread toggle,
        // rather than floating in the center of the available strip.
        const auto buttons = toolbar->findChildren<QToolButton*>(
            QStringLiteral("teamToolbarTeamButton"));
        QVERIFY(!buttons.isEmpty());
        const QPoint firstButtonTopLeft = buttons.first()->mapTo(toolbar, QPoint(0, 0));
        const int unreadRight = unreadOnly->mapTo(
            toolbar, QPoint(unreadOnly->width(), 0)).x();
        QVERIFY(firstButtonTopLeft.x() >= unreadRight);
        QVERIFY2(firstButtonTopLeft.x() - unreadRight <= 30,
                 "Team pills must stay left-aligned after the unread toggle");

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
        auto* browse = toolbar->findChild<QToolButton*>(
            QStringLiteral("teamToolbarBrowse"));
        QVERIFY(browse);
        QVERIFY(!browse->icon().isNull());
        QVERIFY(!beta->text().isEmpty());
        QCOMPARE(beta->text(), QStringLiteral("Beta team"));
        QVERIFY(toolbar->findChild<QWidget*>(
            QStringLiteral("teamToolbarSeparator")));

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
