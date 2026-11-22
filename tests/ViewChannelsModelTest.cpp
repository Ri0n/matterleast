#include <QtTest>

#include "backend/AccountManager.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "workspaces/ViewChannelsModel.h"
#include "workspaces/WorkspaceLayout.h"

using namespace Mattermost;

namespace {

WorkspaceLayout layout()
{
    WorkspaceLayout result;
    result.accounts = {
        {QStringLiteral("alpha"), QStringLiteral("Alpha")},
        {QStringLiteral("beta"), QStringLiteral("Beta")}
    };
    result.profiles = {
        {QStringLiteral("work"), QStringLiteral("Work"),
         {QStringLiteral("alpha"), QStringLiteral("beta")}}
    };
    result.views = {
        {QStringLiteral("merged"), QStringLiteral("work"), QStringLiteral("Merged"),
         {QStringLiteral("alpha"), QStringLiteral("beta")}},
        {QStringLiteral("beta-only"), QStringLiteral("work"), QStringLiteral("Beta"),
         {QStringLiteral("beta")}}
    };
    result.activeViewId = QStringLiteral("merged");
    return result;
}

BackendChannel* populate(Backend& backend, const QString& title)
{
    auto& storage = backend.getStorage();
    storage.addUser(
        QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
    auto* team = storage.addTeam(
        QJsonObject {{"id", "team"}, {"name", "team"},
                     {"display_name", "Engineering"}});
    if (!team)
        return nullptr;
    BackendChannel* channel = storage.addTeamChannel(*team, QJsonObject {
        {"id", "collision"}, {"team_id", "team"},
        {"type", "O"}, {"name", "general"}, {"display_name", title}
    });
    if (!channel)
        return nullptr;

    SidebarCategory category;
    category.id = QStringLiteral("cat");
    category.teamId = QStringLiteral("team");
    category.type = QStringLiteral("channels");
    category.displayName = QStringLiteral("Channels");
    category.sorting = QStringLiteral("manual");
    category.channelIds = QStringList{QStringLiteral("collision")};
    SidebarTeamState state;
    state.order = QStringList{category.id};
    state.categories.insert(category.id, category);
    auto& sidebar = SidebarService::instance(backend);
    sidebar.applyLocalTeamState(team->id, state);
    // setChannelMentioned() operates on an existing activity record, as it
    // does for a real channel after the first membership/view event.
    sidebar.applyServerChannelViewed(*channel, 0);
    return channel;
}

QModelIndex channelIndex(ViewChannelsModel& model, int accountRow)
{
    const QModelIndex account = model.index(accountRow, 0);
    const QModelIndex team = model.index(0, 0, account);
    const QModelIndex category = model.index(0, 0, team);
    return model.index(0, 0, category);
}

} // namespace

class ViewChannelsModelTest final : public QObject
{
    Q_OBJECT
private slots:
    void identicalRemoteIdsNeverMergeAcrossAccounts()
    {
        WorkspaceLayout topology = layout();
        QVERIFY(topology.validate());
        ViewChannelsModel model(topology);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(0, 0).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Alpha"));
        QCOMPARE(model.index(1, 0).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Beta"));
        QCOMPARE(model.rowCount(model.index(0, 0)), 0);
        QVERIFY(!model.index(0, 0).data(ViewChannelsModel::ConnectedRole).toBool());

        Backend first;
        Backend second;
        QVERIFY(populate(first, QStringLiteral("Alpha's general")));
        QVERIFY(populate(second, QStringLiteral("Beta's general")));
        model.setBackend(QStringLiteral("alpha"), &first);
        model.setBackend(QStringLiteral("beta"), &second);

        QCOMPARE(model.rowCount(model.index(0, 0)), 1);
        QCOMPARE(model.rowCount(model.index(1, 0)), 1);
        const QModelIndex a = channelIndex(model, 0);
        const QModelIndex b = channelIndex(model, 1);
        QVERIFY(a.isValid());
        QVERIFY(b.isValid());
        QCOMPARE(a.data(ViewChannelsModel::ChannelIdRole),
                 b.data(ViewChannelsModel::ChannelIdRole));
        QCOMPARE(a.data(ViewChannelsModel::ChannelIdRole).toString(),
                 QStringLiteral("collision"));
        QCOMPARE(a.data(ViewChannelsModel::AccountIdRole).toString(),
                 QStringLiteral("alpha"));
        QCOMPARE(b.data(ViewChannelsModel::AccountIdRole).toString(),
                 QStringLiteral("beta"));
        QVERIFY(a.data(ViewChannelsModel::NodeIdRole)
                    != b.data(ViewChannelsModel::NodeIdRole));
        QCOMPARE(a.data(Qt::DisplayRole).toString(),
                 QStringLiteral("Alpha's general"));
        QCOMPARE(b.data(Qt::DisplayRole).toString(),
                 QStringLiteral("Beta's general"));
        QCOMPARE(model.parent(model.parent(model.parent(a))),
                 model.index(0, 0));
        QCOMPARE(model.roleNames().value(ViewChannelsModel::ChannelIdRole),
                 QByteArrayLiteral("channelId"));
        QCOMPARE(resets.count(), 0);
    }

    void categoryAndReadChangesTouchOnlyTheirAccount()
    {
        WorkspaceLayout topology = layout();
        Backend first;
        Backend second;
        BackendChannel* channel = populate(first, QStringLiteral("Alpha"));
        QVERIFY(channel);
        QVERIFY(populate(second, QStringLiteral("Beta")));
        ViewChannelsModel model(topology);
        model.setBackend(QStringLiteral("alpha"), &first);
        model.setBackend(QStringLiteral("beta"), &second);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy updates(&model, &QAbstractItemModel::dataChanged);
        const QPersistentModelIndex beta = channelIndex(model, 1);
        QVERIFY(beta.isValid());

        SidebarService::instance(first).setChannelMentioned(
            QStringLiteral("collision"), true);
        QCOMPARE(resets.count(), 0);
        QVERIFY(channelIndex(model, 0)
                    .data(ViewChannelsModel::MentionedRole).toBool());
        QVERIFY(!beta.data(ViewChannelsModel::MentionedRole).toBool());
        QVERIFY(updates.count() >= 1);
        bool sawMentionUpdate = false;
        for (const auto& change : updates) {
            const QModelIndex changed = change.at(0).value<QModelIndex>();
            QCOMPARE(changed.data(ViewChannelsModel::AccountIdRole).toString(),
                     QStringLiteral("alpha"));
            const QVector<int> roles = change.at(2).value<QVector<int>>();
            sawMentionUpdate = sawMentionUpdate
                || roles.contains(ViewChannelsModel::MentionedRole);
            QVERIFY(!roles.contains(Qt::DisplayRole));
        }
        QVERIFY(sawMentionUpdate);
        QVERIFY(model.index(0, 0).data(ViewChannelsModel::MentionedRole).toBool());
        QVERIFY(!model.index(1, 0).data(ViewChannelsModel::MentionedRole).toBool());

        auto& storage = first.getStorage();
        BackendChannel* added = storage.addTeamChannel(
            *storage.getTeamById(QStringLiteral("team")),
            QJsonObject {{"id", "new-channel"}, {"team_id", "team"},
                         {"type", "O"}, {"name", "new"},
                         {"display_name", "New channel"}});
        QVERIFY(added);
        auto& sidebar = SidebarService::instance(first);
        SidebarTeamState state = *sidebar.teamState(QStringLiteral("team"));
        state.categories[QStringLiteral("cat")].channelIds.append(
            QStringLiteral("new-channel"));
        sidebar.applyLocalTeamState(QStringLiteral("team"), state);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(model.rowCount(model.parent(channelIndex(model, 0))), 2);
        QVERIFY(beta.isValid());
        QCOMPARE(beta.data(Qt::DisplayRole).toString(), QStringLiteral("Beta"));
        QCOMPARE(beta.data(ViewChannelsModel::AccountIdRole).toString(),
                 QStringLiteral("beta"));
    }

    void viewSelectionAndBackendReplacementPreserveStableRows()
    {
        WorkspaceLayout topology = layout();
        Backend first;
        Backend second;
        QVERIFY(populate(first, QStringLiteral("Alpha")));
        QVERIFY(populate(second, QStringLiteral("Beta")));
        ViewChannelsModel model(topology);
        model.setBackend(QStringLiteral("alpha"), &first);
        model.setBackend(QStringLiteral("beta"), &second);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
        QSignalSpy selection(&model, &ViewChannelsModel::activeViewChanged);
        const QPersistentModelIndex betaAccount = model.index(1, 0);
        QVERIFY(betaAccount.isValid());

        QVERIFY(topology.selectView(QStringLiteral("beta-only")));
        model.syncView();
        QCOMPARE(model.activeViewId(), QStringLiteral("beta-only"));
        QCOMPARE(selection.count(), 1);
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(moved.count(), 1);
        QVERIFY(betaAccount.isValid());
        QCOMPARE(betaAccount.row(), 0);
        QCOMPARE(betaAccount.data(ViewChannelsModel::AccountIdRole).toString(),
                 QStringLiteral("beta"));
        QCOMPARE(model.rowCount(betaAccount), 1);

        // An offline account remains a selectable identity, without stale
        // channel objects after its Backend is discarded.
        model.setBackend(QStringLiteral("beta"), nullptr);
        QCOMPARE(model.rowCount(betaAccount), 0);
        QVERIFY(!betaAccount.data(ViewChannelsModel::ConnectedRole).toBool());

        QVERIFY(topology.selectView(QStringLiteral("merged")));
        model.syncView();
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(1, 0).data(ViewChannelsModel::AccountIdRole).toString(),
                 QStringLiteral("beta"));
        QCOMPARE(model.rowCount(model.index(1, 0)), 0);
        model.setBackend(QStringLiteral("beta"), &second);
        QCOMPARE(model.rowCount(model.index(1, 0)), 1);
        QCOMPARE(channelIndex(model, 1).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Beta"));
        QCOMPARE(resets.count(), 0);
    }

    void managerSynchronizesVisibleMembershipAndLabels()
    {
        AccountManager manager;
        WorkspaceLayout topology = layout();
        QVERIFY(manager.load(topology.toJson()));
        // Ordinary single-account desktop startup need not allocate this
        // parallel projection. It is created on first frontend access.
        QVERIFY(!manager.findChild<ViewChannelsModel*>());
        auto* model = qobject_cast<ViewChannelsModel*>(manager.activeViewChannels());
        QVERIFY(model);
        QCOMPARE(manager.activeViewChannels(), static_cast<QAbstractItemModel*>(model));
        QCOMPARE(model->rowCount(), 2);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        QSignalSpy selected(model, &ViewChannelsModel::activeViewChanged);

        QVERIFY(manager.activateView(QStringLiteral("beta-only")));
        QCOMPARE(model->activeViewId(), QStringLiteral("beta-only"));
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 0).data(ViewChannelsModel::AccountIdRole).toString(),
                 QStringLiteral("beta"));

        QVERIFY(manager.activateView(QStringLiteral("merged")));
        QCOMPARE(model->rowCount(), 2);
        QCOMPARE(selected.count(), 2);
        WorkspaceLayout renamed = manager.layout();
        renamed.accounts[0].name = QStringLiteral("Renamed Alpha");
        QVERIFY(manager.applyLayoutEdits(renamed));
        QCOMPARE(model->index(0, 0).data(Qt::DisplayRole).toString(),
                 QStringLiteral("Renamed Alpha"));
        QCOMPARE(resets.count(), 0);
    }

    void sessionEndingClearsProjectionBeforeStorageReset()
    {
        WorkspaceLayout topology = layout();
        ViewChannelsModel model(topology);
        Backend backend;
        QVERIFY(populate(backend, QStringLiteral("Temporary")));
        model.setBackend(QStringLiteral("alpha"), &backend);
        QCOMPARE(model.rowCount(model.index(0, 0)), 1);
        backend.sessionEnding();
        QCOMPARE(model.rowCount(model.index(0, 0)), 0);
        QVERIFY(!model.index(0, 0)
                    .data(ViewChannelsModel::ConnectedRole).toBool());
        // Reused Backend objects can be rebound after explicit recovery.
        model.setBackend(QStringLiteral("alpha"), &backend);
        QCOMPARE(model.rowCount(model.index(0, 0)), 1);
    }

    void destroyedBackendCannotLeaveStaleChannels()
    {
        WorkspaceLayout topology = layout();
        ViewChannelsModel model(topology);
        auto* backend = new Backend;
        QVERIFY(populate(*backend, QStringLiteral("Temporary")));
        model.setBackend(QStringLiteral("alpha"), backend);
        QCOMPARE(model.rowCount(model.index(0, 0)), 1);
        delete backend;
        QCOMPARE(model.rowCount(model.index(0, 0)), 0);
        QVERIFY(!model.index(0, 0)
                     .data(ViewChannelsModel::ConnectedRole).toBool());
    }
};

QTEST_MAIN(ViewChannelsModelTest)
#include "ViewChannelsModelTest.moc"
