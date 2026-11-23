#include <QtTest>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "workspaces/ViewConversationModel.h"
#include "workspaces/ViewSidebarModel.h"

using namespace Mattermost;

namespace {

void populate(Backend& backend, const QString& accountTitle,
              const QString& teamId, const QString& teamTitle)
{
    auto& storage = backend.getStorage();
    storage.addUser({{QStringLiteral("id"), QStringLiteral("self")},
                     {QStringLiteral("username"), QStringLiteral("self")}}, true);
    storage.addTeam({{QStringLiteral("id"), teamId},
                     {QStringLiteral("display_name"), teamTitle}});
    storage.addOwnChannel({
        {QStringLiteral("id"), QStringLiteral("same-channel")},
        {QStringLiteral("team_id"), teamId},
        {QStringLiteral("type"), QStringLiteral("O")},
        {QStringLiteral("display_name"), accountTitle + QStringLiteral(" public")}});
    storage.addOwnChannel({
        {QStringLiteral("id"), QStringLiteral("same-dm")},
        {QStringLiteral("type"), QStringLiteral("G")},
        {QStringLiteral("last_post_at"), 100},
        {QStringLiteral("display_name"), accountTitle + QStringLiteral(" DM")}});
    SidebarTeamState state;
    SidebarCategory channels;
    channels.id = QStringLiteral("channels");
    channels.type = QStringLiteral("channels");
    channels.displayName = QStringLiteral("Channels");
    channels.sorting = QStringLiteral("manual");
    channels.channelIds = QStringList{QStringLiteral("same-channel")};
    SidebarCategory dm;
    dm.id = QStringLiteral("direct");
    dm.type = QStringLiteral("direct_messages");
    dm.channelIds = QStringList{QStringLiteral("same-dm")};
    state.order = QStringList{channels.id, dm.id};
    state.categories.insert(channels.id, channels);
    state.categories.insert(dm.id, dm);
    SidebarService::instance(backend).applyLocalTeamState(teamId, state);
}

} // namespace

class ViewSidebarModelTest final : public QObject
{
    Q_OBJECT
private slots:
    void twoServersHaveIndependentChannelsAndSharedDirectMessages()
    {
        Backend first;
        Backend second;
        populate(first, QStringLiteral("Alpha"), QStringLiteral("t1"),
                 QStringLiteral("Engineering"));
        populate(second, QStringLiteral("Beta"), QStringLiteral("t2"),
                 QStringLiteral("Operations"));

        ViewConversationModel conversations;
        conversations.setSources({{QStringLiteral("a"), &first},
                                  {QStringLiteral("b"), &second}});
        ViewSidebarModel model;
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        model.setSources({{QStringLiteral("a"), QStringLiteral("Alpha"), &first},
                          {QStringLiteral("b"), QStringLiteral("Beta"), &second}},
                         &conversations);

        QCOMPARE(model.rowCount(), 3);
        const QModelIndex accountA = model.index(0, 0);
        const QModelIndex accountB = model.index(1, 0);
        const QModelIndex directs = model.index(2, 0);
        QCOMPARE(accountA.data(ViewSidebarModel::KindRole).toInt(),
                 int(ViewSidebarModel::Account));
        QCOMPARE(accountB.data(ViewSidebarModel::AccountIdRole).toString(),
                 QStringLiteral("b"));
        QCOMPARE(directs.data(ViewSidebarModel::KindRole).toInt(),
                 int(ViewSidebarModel::SharedDirectMessages));
        QCOMPARE(model.rowCount(directs), 2);
        QCOMPARE(model.rowCount(accountA), 1);
        QCOMPARE(model.rowCount(accountB), 1);
        const auto alphaChannel = model.findChannel(QStringLiteral("a"),
                                                    QStringLiteral("same-channel"));
        const auto betaChannel = model.findChannel(QStringLiteral("b"),
                                                   QStringLiteral("same-channel"));
        QVERIFY(alphaChannel.isValid());
        QVERIFY(betaChannel.isValid());
        QVERIFY(alphaChannel != betaChannel);
        QCOMPARE(alphaChannel.data(Qt::DisplayRole).toString(),
                 QStringLiteral("Alpha public"));
        QCOMPARE(betaChannel.data(Qt::DisplayRole).toString(),
                 QStringLiteral("Beta public"));
        QCOMPARE(model.parent(model.parent(model.parent(alphaChannel))), accountA);
        QCOMPARE(model.parent(model.parent(model.parent(betaChannel))), accountB);
        QCOMPARE(model.rowCount(model.parent(alphaChannel)), 1);
        QCOMPARE(model.index(0, 0, model.parent(alphaChannel))
                     .data(ViewSidebarModel::CategoryIdRole).toString(),
                 QStringLiteral("channels"));
        QVERIFY(model.findChannel(QStringLiteral("a"), QStringLiteral("same-dm")).isValid());
        QVERIFY(model.findChannel(QStringLiteral("b"), QStringLiteral("same-dm")).isValid());
        QCOMPARE(model.roleNames().value(ViewSidebarModel::AccountIdRole),
                 QByteArrayLiteral("accountId"));
        QCOMPARE(resets.count(), 0);
    }

    void categoryAndMembershipChangesReconcileWithoutModelReset()
    {
        Backend first;
        Backend second;
        populate(first, QStringLiteral("Alpha"), QStringLiteral("t1"),
                 QStringLiteral("Engineering"));
        populate(second, QStringLiteral("Beta"), QStringLiteral("t2"),
                 QStringLiteral("Operations"));
        ViewConversationModel conversations;
        conversations.setSources({{QStringLiteral("a"), &first},
                                  {QStringLiteral("b"), &second}});
        ViewSidebarModel model;
        model.setSources({{QStringLiteral("a"), QStringLiteral("Alpha"), &first},
                          {QStringLiteral("b"), QStringLiteral("Beta"), &second}},
                         &conversations);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);

        auto state = *SidebarService::instance(first).teamState(QStringLiteral("t1"));
        state.categories[QStringLiteral("channels")].displayName
            = QStringLiteral("Updated channels");
        SidebarService::instance(first).applyLocalTeamState(
            QStringLiteral("t1"), state);
        QCOMPARE(model.index(0, 0, model.index(0, 0, model.index(0, 0)))
                     .data(Qt::DisplayRole).toString(), QStringLiteral("Updated channels"));
        QVERIFY(changed.count() > 0);
        // The disconnected server's root stays visible but its child data is
        // released; no other server is allowed to claim the same remote ID.
        model.setSources({{QStringLiteral("a"), QStringLiteral("Alpha"), &first},
                          {QStringLiteral("b"), QStringLiteral("Beta"), nullptr}},
                         &conversations);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.rowCount(model.index(1, 0)), 0);
        QVERIFY(!model.findChannel(QStringLiteral("b"), QStringLiteral("same-channel")).isValid());
        QVERIFY(model.findChannel(QStringLiteral("a"), QStringLiteral("same-channel")).isValid());
        QVERIFY(removed.count() > 0);
        QCOMPARE(resets.count(), 0);

        // Existing singleton Widgets presentation remains responsible for its
        // original server sidebar: the projection has no wrapper nodes.
        model.setSources({{QStringLiteral("a"), QStringLiteral("Alpha"), &first}},
                         &conversations);
        QCOMPARE(model.rowCount(), 0);
        QCOMPARE(resets.count(), 0);
    }
};

QTEST_MAIN(ViewSidebarModelTest)
#include "ViewSidebarModelTest.moc"
