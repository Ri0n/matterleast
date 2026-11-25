#include <QtTest>
#include "navigation/ViewNavigationSession.h"
#include "migrations/NavigationSnapshotMigration.h"

using namespace Mattermost;

namespace {
ViewNavigationSession::Location chat(const QString& account,
                                     const QString& channel,
                                     const QString& root = {})
{
    ViewNavigationSession::Location result;
    result.accountId = account;
    result.channelId = channel;
    result.rootId = root;
    return result;
}
}

class ViewNavigationSessionTest final : public QObject
{
    Q_OBJECT
private slots:
    void serializedViewsSurviveRestartWithAccountQualifiedBookmarks()
    {
        ViewNavigationSession original(QStringLiteral("merged"));
        original.setAllowedAccounts({"office", "home"});
        auto office = chat("office", "shared");
        office.postId = "post-office";
        office.viewportAnchor = NavigationTabsModel::ViewportAnchor {
            "post-office", false, qreal(0.375)};
        auto home = chat("home", "shared", "thread-home");
        home.postId = "post-home";
        home.viewportAnchor = NavigationTabsModel::ViewportAnchor {
            "post-home", true, qreal(0.25)};
        QCOMPARE(original.openTab(office, "Office", true), 0);
        QCOMPARE(original.openTab(home, "Home"), 1);
        QVERIFY(original.goBack().has_value());
        auto state = original.snapshot();
        state.dock = home;
        state.dockVisible = true;
        QVERIFY(original.restoreSnapshot(state));
        const QByteArray json = original.serializeSnapshot();

        ViewNavigationSession restored(QStringLiteral("merged"));
        restored.setAllowedAccounts({"office", "home"});
        QSignalSpy reset(&restored, &QAbstractItemModel::modelReset);
        QVERIFY(restored.restoreSerializedSnapshot(json));
        QCOMPARE(restored.rowCount(), 2);
        QCOMPARE(restored.activeTab(), 0);
        QCOMPARE(restored.tabs().at(0)->accountId, QStringLiteral("office"));
        QCOMPARE(restored.tabs().at(1)->accountId, QStringLiteral("home"));
        QVERIFY(restored.tabs().at(0)->pinned);
        QCOMPARE(restored.tabs().at(0)->viewportAnchor->fraction, qreal(0.375));
        QCOMPARE(restored.tabs().at(1)->viewportAnchor->bottom, true);
        QCOMPARE(restored.snapshot().dock.rootId, QStringLiteral("thread-home"));
        QVERIFY(restored.snapshot().dockVisible);
        QVERIFY(!restored.snapshot().forward.isEmpty());
        QCOMPARE(reset.count(), 0);

        const QByteArray previous = restored.serializeSnapshot();
        QVERIFY(!restored.restoreSerializedSnapshot(
            QByteArrayLiteral(R"({"version":2})")));
        QVERIFY(!restored.restoreSerializedSnapshot(
            QByteArrayLiteral(R"({"version":1,"view":"wrong"})")));
        QCOMPARE(restored.serializeSnapshot(), previous);
        ViewNavigationSession restricted(QStringLiteral("merged"));
        restricted.setAllowedAccounts({"office"});
        QVERIFY(!restricted.restoreSerializedSnapshot(json));
        QCOMPARE(restricted.rowCount(), 0);
    }

    void legacyMigrationImportsOnlyPinnedTabs()
    {
        ViewNavigationSession view(QStringLiteral("combined"));
        view.setAllowedAccounts({"office", "home"});
        QVERIFY(view.openTab(chat("home", "shared"), "Home") >= 0);
        const QByteArray old = R"({"version":2,"tabs":[{"channel":"shared","root":"","pinned":true,"anchor":{"post":"p1","bottom":false,"fraction":0.5}},{"channel":"ordinary","root":"","pinned":false},{"channel":"shared","root":"pinned-thread","pinned":true}],"threads":[{"channel":"shared","root":"root1","mode":"window","geometry":""}],"active_tab":{"channel":"ordinary","root":""},"active_dock":{"channel":"shared","root":"root2"},"dock_visible":true})";
        QVERIFY(NavigationSnapshotMigration::importLegacy(old, "office", view));
        QCOMPARE(view.rowCount(), 3);
        QCOMPARE(view.tabs().at(0)->accountId, QStringLiteral("home"));
        QCOMPARE(view.tabs().at(1)->accountId, QStringLiteral("office"));
        QCOMPARE(view.tabs().at(1)->channelId, QStringLiteral("shared"));
        QVERIFY(view.tabs().at(1)->pinned);
        QVERIFY(view.tabs().at(1)->postId.isEmpty());
        QVERIFY(!view.tabs().at(1)->viewportAnchor.has_value());
        QCOMPARE(view.tabs().at(2)->rootId, QStringLiteral("pinned-thread"));
        QVERIFY(view.tabs().at(2)->pinned);
        QCOMPARE(view.activeTab(), 0);
        QVERIFY(view.snapshot().detached.isEmpty());
        QVERIFY(!view.snapshot().dockVisible);
        QVERIFY(view.snapshot().back.isEmpty());
        QVERIFY(view.snapshot().forward.isEmpty());
        QVERIFY(NavigationSnapshotMigration::importLegacy(old, "office", view));
        QCOMPARE(view.rowCount(), 3);

        const QByteArray encoded = view.serializeSnapshot();
        ViewNavigationSession reopened(QStringLiteral("combined"));
        reopened.setAllowedAccounts({"office", "home"});
        QVERIFY(reopened.restoreSerializedSnapshot(encoded));
        QCOMPARE(reopened.rowCount(), 3);
        QVERIFY(reopened.tabs().at(1)->pinned);
        QVERIFY(reopened.tabs().at(2)->pinned);
        const QByteArray before = view.serializeSnapshot();
        QVERIFY(!NavigationSnapshotMigration::importLegacy(
            QByteArrayLiteral(R"({"version":9,"tabs":[]})"),
            "office", view));
        QCOMPARE(view.serializeSnapshot(), before);
    }

    void detachedWindowsSurviveNewFormatRoundTrip()
    {
        ViewNavigationSession session(QStringLiteral("all"));
        session.setAllowedAccounts({"office", "home"});
        auto state = session.snapshot();
        auto location = chat("office", "shared", "root1");
        location.postId = "post1";
        location.viewportAnchor = NavigationTabsModel::ViewportAnchor {
            "post1", true, qreal(0.25)};
        state.detached.append({location, QByteArray("geometry")});
        QVERIFY(session.restoreSnapshot(state));
        ViewNavigationSession reopened(QStringLiteral("all"));
        reopened.setAllowedAccounts({"office", "home"});
        QVERIFY(reopened.restoreSerializedSnapshot(session.serializeSnapshot()));
        QCOMPARE(reopened.snapshot().detached.size(), 1);
        const auto detached = reopened.snapshot().detached.first();
        QCOMPARE(detached.location.accountId, QStringLiteral("office"));
        QCOMPARE(detached.location.rootId, QStringLiteral("root1"));
        QCOMPARE(detached.geometry, QByteArray("geometry"));
        QVERIFY(detached.location.viewportAnchor.has_value());
        QCOMPARE(detached.location.viewportAnchor->fraction, qreal(0.25));
    }

    void removingAccountPrunesDetachedWindowsWithoutModelReset()
    {
        ViewNavigationSession session(QStringLiteral("all"));
        session.setAllowedAccounts({"office", "home"});
        auto state = session.snapshot();
        state.detached.append({chat("office", "shared", "root1"), QByteArray("geometry")});
        state.detached.append({chat("home", "shared", "root1"), QByteArray("geometry")});
        QVERIFY(session.restoreSnapshot(state));
        QSignalSpy reset(&session, &QAbstractItemModel::modelReset);
        session.setAllowedAccounts({"home"});
        QCOMPARE(session.snapshot().detached.size(), 1);
        QCOMPARE(session.snapshot().detached.first().location.accountId,
                 QStringLiteral("home"));
        QCOMPARE(reset.count(), 0);
    }

    void sameChannelIdFromTwoAccountsNeverCollides()
    {
        ViewNavigationSession session(QStringLiteral("merged"));
        QSignalSpy added(&session, &QAbstractItemModel::rowsInserted);
        QSignalSpy reset(&session, &QAbstractItemModel::modelReset);
        QCOMPARE(session.openTab(chat(QStringLiteral("office"), QStringLiteral("shared")),
                                 QStringLiteral("Office")), 0);
        QCOMPARE(session.openTab(chat(QStringLiteral("home"), QStringLiteral("shared")),
                                 QStringLiteral("Home")), 1);
        QCOMPARE(session.rowCount(), 2);
        QCOMPARE(session.tabs().findDestination(QStringLiteral("shared"), {},
                                                 QStringLiteral("office")), 0);
        QCOMPARE(session.tabs().findDestination(QStringLiteral("shared"), {},
                                                 QStringLiteral("home")), 1);
        QCOMPARE(session.data(session.index(0, 0),
                              ViewNavigationSession::AccountIdRole).toString(),
                 QStringLiteral("office"));
        QCOMPARE(session.data(session.index(1, 0),
                              ViewNavigationSession::AccountIdRole).toString(),
                 QStringLiteral("home"));
        QCOMPARE(session.openTab(chat(QStringLiteral("office"), QStringLiteral("shared"))),
                 0);
        QCOMPARE(session.rowCount(), 2);
        QCOMPARE(added.count(), 2);
        QCOMPARE(reset.count(), 0);
    }

    void separateViewsRetainIndependentHistoryAndPinnedTabs()
    {
        ViewNavigationSession merged(QStringLiteral("merged"));
        ViewNavigationSession personal(QStringLiteral("personal"));
        QSignalSpy mergedReset(&merged, &QAbstractItemModel::modelReset);
        QSignalSpy personalReset(&personal, &QAbstractItemModel::modelReset);
        QVERIFY(!merged.viewId().isEmpty());
        QCOMPARE(merged.openTab(chat(QStringLiteral("office"), QStringLiteral("main")),
                                QStringLiteral("Main"), true), 0);
        QCOMPARE(merged.openTab(chat(QStringLiteral("home"), QStringLiteral("dm")),
                                QStringLiteral("DM")), 1);
        QCOMPARE(personal.openTab(chat(QStringLiteral("home"), QStringLiteral("dm")),
                                  QStringLiteral("Home DM")), 0);

        QCOMPARE(merged.activeTab(), 1);
        QCOMPARE(personal.activeTab(), 0);
        QVERIFY(merged.canGoBack());
        QVERIFY(!personal.canGoBack());
        QCOMPARE(merged.goBack()->accountId, QStringLiteral("office"));
        QCOMPARE(merged.activeTab(), 0);
        QCOMPARE(merged.goForward()->accountId, QStringLiteral("home"));
        QCOMPARE(merged.activeTab(), 1);
        QCOMPARE(merged.data(merged.index(0, 0),
                             ViewNavigationSession::PinnedRole).toBool(), true);
        QVERIFY(merged.setPinned(0, false));
        QCOMPARE(merged.data(merged.index(0, 0),
                             ViewNavigationSession::PinnedRole).toBool(), false);
        QCOMPARE(personal.rowCount(), 1);
        QCOMPARE(merged.rowCount(), 2);
        QCOMPARE(mergedReset.count(), 0);
        QCOMPARE(personalReset.count(), 0);
    }

    void historyIncludesCollectionsAndUsesAccountQualifiedLocations()
    {
        ViewNavigationSession session(QStringLiteral("all"));
        auto office = chat(QStringLiteral("office"), QStringLiteral("x"));
        auto home = chat(QStringLiteral("home"), QStringLiteral("x"));
        ViewNavigationSession::Location saved;
        saved.accountId = QStringLiteral("office");
        saved.kind = ViewNavigationSession::Location::Kind::Saved;
        QVERIFY(session.visit(office));
        QVERIFY(session.visit(saved));
        QVERIFY(session.visit(home));
        QVERIFY(session.goBack()->kind == ViewNavigationSession::Location::Kind::Saved);
        QCOMPARE(session.goBack()->accountId, QStringLiteral("office"));
        QVERIFY(session.goForward()->kind == ViewNavigationSession::Location::Kind::Saved);
        QVERIFY(session.visit(home));
        QVERIFY(!session.canGoForward());
        QVERIFY(!session.visit(ViewNavigationSession::Location{}));
    }

    void bookmarkAndInvalidOperationDoNotRebuildModel()
    {
        ViewNavigationSession session(QStringLiteral("all"));
        QSignalSpy reset(&session, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&session, &QAbstractItemModel::dataChanged);
        const auto a = chat(QStringLiteral("office"), QStringLiteral("c"));
        QCOMPARE(session.openTab(a), 0);
        const NavigationTabsModel::ViewportAnchor anchor {
            QStringLiteral("post-42"), true, 0.42};
        QVERIFY(session.updateBookmark(0, QStringLiteral("post-42"), anchor));
        const auto* entry = session.tabs().at(0);
        QVERIFY(entry);
        QVERIFY(entry->viewportAnchor.has_value());
        QCOMPARE(entry->viewportAnchor->postId, QStringLiteral("post-42"));
        QCOMPARE(entry->viewportAnchor->fraction, 0.42);
        QVERIFY(!session.setPinned(4, true));
        QVERIFY(!session.closeTab(-1));
        QVERIFY(!session.activateTab(3));
        QVERIFY(session.closeTab(0));
        QCOMPARE(session.activeTab(), -1);
        QCOMPARE(session.rowCount(), 0);
        QCOMPARE(reset.count(), 0);
        QVERIFY(changed.count() >= 1);
    }

    void stateTransferKeepsAccountsDistinctAndUsesIncrementalSignals()
    {
        ViewNavigationSession from(QStringLiteral("view-a"));
        auto alpha = chat(QStringLiteral("a"), QStringLiteral("same"));
        auto beta = chat(QStringLiteral("b"), QStringLiteral("same"));
        auto other = chat(QStringLiteral("a"), QStringLiteral("other"));
        auto gamma = chat(QStringLiteral("b"), QStringLiteral("third"));
        QCOMPARE(from.openTab(alpha, QStringLiteral("Alpha"), true), 0);
        QCOMPARE(from.openTab(beta, QStringLiteral("Beta")), 1);
        QCOMPARE(from.openTab(gamma, QStringLiteral("Gamma")), 2);
        QVERIFY(from.visit(other));
        auto transfer = from.snapshot();
        QCOMPARE(transfer.activeTab, -1);
        QCOMPARE(transfer.tabs.size(), 3);
        QCOMPARE(transfer.back.size(), 3);

        ViewNavigationSession target(QStringLiteral("view-b"));
        QCOMPARE(target.openTab(beta, QStringLiteral("Old")), 0);
        QCOMPARE(target.openTab(other, QStringLiteral("Remove")), 1);
        QCOMPARE(target.openTab(alpha, QStringLiteral("Old Alpha")), 2);
        QSignalSpy reset(&target, &QAbstractItemModel::modelReset);
        QSignalSpy moved(&target, &QAbstractItemModel::rowsMoved);
        QSignalSpy inserted(&target, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&target, &QAbstractItemModel::rowsRemoved);
        QVERIFY(target.restoreSnapshot(transfer));
        QCOMPARE(target.viewId(), QStringLiteral("view-b"));
        QCOMPARE(target.rowCount(), 3);
        QCOMPARE(target.tabs().at(0)->accountId, QStringLiteral("a"));
        QCOMPARE(target.tabs().at(0)->pinned, true);
        QCOMPARE(target.tabs().at(1)->accountId, QStringLiteral("b"));
        QCOMPARE(target.tabs().at(1)->title, QStringLiteral("Beta"));
        QCOMPARE(target.currentLocation().channelId, QStringLiteral("other"));
        QVERIFY(target.canGoBack());
        QCOMPARE(reset.count(), 0);
        QCOMPARE(moved.count(), 1);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(target.snapshot().back.size(), transfer.back.size());
    }

    void ordinaryNavigationReusesOnlyUnpinnedChannelTabs()
    {
        ViewNavigationSession session(QStringLiteral("combined"));
        const auto a = chat(QStringLiteral("a"), QStringLiteral("same"));
        const auto b = chat(QStringLiteral("b"), QStringLiteral("same"));
        const auto c = chat(QStringLiteral("b"), QStringLiteral("other"));
        QCOMPARE(session.openTab(a, QStringLiteral("Pinned"), true), 0);
        QCOMPARE(session.reuseOrOpenChannelTab(b, QStringLiteral("Other account")), 1);
        QCOMPARE(session.rowCount(), 2);
        QCOMPARE(session.reuseOrOpenChannelTab(c, QStringLiteral("Reused")), 1);
        QCOMPARE(session.rowCount(), 2);
        QCOMPARE(session.tabs().at(0)->accountId, QStringLiteral("a"));
        QCOMPARE(session.tabs().at(1)->channelId, QStringLiteral("other"));
        QCOMPARE(session.openTab(b, QStringLiteral("Explicit new tab")), 2);
        QCOMPARE(session.rowCount(), 3);
        QCOMPARE(session.reuseOrOpenChannelTab(a), 0);
        QCOMPARE(session.tabs().at(0)->pinned, true);
    }

    void tabReorderingKeepsActiveDestinationAndHistory()
    {
        ViewNavigationSession session(QStringLiteral("combined"));
        const auto a = chat(QStringLiteral("a"), QStringLiteral("a"));
        const auto b = chat(QStringLiteral("a"), QStringLiteral("b"));
        const auto c = chat(QStringLiteral("b"), QStringLiteral("c"));
        QCOMPARE(session.openTab(a), 0);
        QCOMPARE(session.openTab(b), 1);
        QCOMPARE(session.openTab(c), 2);
        QSignalSpy moved(&session, &QAbstractItemModel::rowsMoved);
        QSignalSpy reset(&session, &QAbstractItemModel::modelReset);
        QVERIFY(session.moveTab(2, 0));
        QCOMPARE(session.activeTab(), 0);
        QCOMPARE(session.tabs().at(0)->channelId, QStringLiteral("c"));
        QVERIFY(session.moveTab(2, 1));
        QCOMPARE(session.activeTab(), 0);
        QCOMPARE(session.currentLocation().channelId, QStringLiteral("c"));
        QCOMPARE(moved.count(), 2);
        QCOMPARE(reset.count(), 0);
        QVERIFY(!session.moveTab(-1, 0));
    }

    void changingViewMembershipPrunesOnlyExcludedAccountState()
    {
        ViewNavigationSession view(QStringLiteral("combined"));
        auto alpha = chat(QStringLiteral("alpha"), QStringLiteral("same"));
        auto beta = chat(QStringLiteral("beta"), QStringLiteral("same"));
        auto thread = chat(QStringLiteral("alpha"), QStringLiteral("same"),
                           QStringLiteral("thread"));
        view.setAllowedAccounts({QStringLiteral("alpha"), QStringLiteral("beta")});
        QCOMPARE(view.openTab(alpha), 0);
        QCOMPARE(view.openTab(beta), 1);
        QVERIFY(view.recordVisit(thread));
        auto state = view.snapshot();
        state.dock = thread;
        state.dockVisible = true;
        QVERIFY(view.restoreSnapshot(state));

        QSignalSpy reset(&view, &QAbstractItemModel::modelReset);
        QSignalSpy removed(&view, &QAbstractItemModel::rowsRemoved);
        view.setAllowedAccounts({QStringLiteral("beta")});
        QCOMPARE(view.rowCount(), 1);
        QCOMPARE(view.tabs().at(0)->accountId, QStringLiteral("beta"));
        QVERIFY(!view.currentLocation().valid());
        QCOMPARE(view.snapshot().back.size(), 1);
        QCOMPARE(view.snapshot().back.front().accountId, QStringLiteral("beta"));
        QVERIFY(!view.snapshot().dockVisible);
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QVERIFY(!view.recordVisit(alpha));
        QCOMPARE(view.openTab(alpha), -1);
        // A stale widget cannot reintroduce another account through a bulk
        // snapshot either, even if it had a valid bookmark before the edit.
        auto stale = view.snapshot();
        NavigationTabsModel::Entry excluded;
        excluded.channelId = QStringLiteral("other");
        excluded.accountId = QStringLiteral("alpha");
        stale.tabs.append(excluded);
        QVERIFY(!view.restoreSnapshot(stale));
        QCOMPARE(view.rowCount(), 1);
        QCOMPARE(reset.count(), 0);
    }

    void dockHistoryDoesNotChangeCentralTabAndBookmarksDoNotAddSteps()
    {
        ViewNavigationSession session(QStringLiteral("view"));
        auto channel = chat(QStringLiteral("account"), QStringLiteral("channel"));
        auto thread = chat(QStringLiteral("account"), QStringLiteral("channel"),
                           QStringLiteral("thread"));
        QCOMPARE(session.openTab(channel), 0);
        QVERIFY(session.recordVisit(thread));
        QCOMPARE(session.activeTab(), 0);
        QCOMPARE(session.currentLocation().rootId, QStringLiteral("thread"));
        QCOMPARE(session.snapshot().back.size(), 1);
        thread.postId = QStringLiteral("last-read-post");
        QVERIFY(session.updateCurrentBookmark(thread));
        QCOMPARE(session.snapshot().back.size(), 1);
        QCOMPARE(session.currentLocation().postId, QStringLiteral("last-read-post"));
        auto unrelated = chat(QStringLiteral("account"), QStringLiteral("unrelated"));
        QVERIFY(!session.updateCurrentBookmark(unrelated));
        QCOMPARE(session.currentLocation().rootId, QStringLiteral("thread"));
        const auto back = session.goBack();
        QVERIFY(back.has_value());
        QCOMPARE(back->rootId, QString());
        QCOMPARE(session.snapshot().forward.size(), 1);
        QCOMPARE(session.snapshot().forward.front().postId,
                 QStringLiteral("last-read-post"));
    }

    void unchangedSnapshotDoesNotToggleActiveTab()
    {
        ViewNavigationSession session(QStringLiteral("view"));
        const auto location = chat(QStringLiteral("account"), QStringLiteral("channel"));
        QCOMPARE(session.openTab(location, QStringLiteral("Channel")), 0);
        QSignalSpy active(&session, &ViewNavigationSession::activeTabChanged);
        QSignalSpy reset(&session, &QAbstractItemModel::modelReset);
        for (int i = 0; i < 3; ++i)
            QVERIFY(session.restoreSnapshot(session.snapshot()));
        QCOMPARE(session.activeTab(), 0);
        QCOMPARE(active.count(), 0);
        QCOMPARE(reset.count(), 0);
    }

    void dockPresentationSurvivesViewSnapshotTransfer()
    {
        ViewNavigationSession source(QStringLiteral("one"));
        auto state = source.snapshot();
        state.dock = chat(QStringLiteral("a"), QStringLiteral("channel"),
                          QStringLiteral("thread"));
        state.dockVisible = true;
        QVERIFY(source.restoreSnapshot(state));
        ViewNavigationSession restored(QStringLiteral("two"));
        QVERIFY(restored.restoreSnapshot(source.snapshot()));
        QCOMPARE(restored.snapshot().dock.accountId, QStringLiteral("a"));
        QCOMPARE(restored.snapshot().dock.rootId, QStringLiteral("thread"));
        QVERIFY(restored.snapshot().dockVisible);
        state.dock.rootId.clear();
        QVERIFY(!restored.restoreSnapshot(state));
        QVERIFY(restored.snapshot().dockVisible);
    }

    void rejectedSnapshotIsAtomic()
    {
        ViewNavigationSession session(QStringLiteral("view"));
        QCOMPARE(session.openTab(chat(QStringLiteral("a"), QStringLiteral("c"))), 0);
        auto invalid = session.snapshot();
        invalid.tabs.append(invalid.tabs.front());
        QSignalSpy reset(&session, &QAbstractItemModel::modelReset);
        QSignalSpy rows(&session, &QAbstractItemModel::rowsInserted);
        QVERIFY(!session.restoreSnapshot(invalid));
        QCOMPARE(session.rowCount(), 1);
        QCOMPARE(session.activeTab(), 0);
        QCOMPARE(rows.count(), 0);
        QCOMPARE(reset.count(), 0);
    }

    void historyIsBoundedWithoutEvictingActiveTab()
    {
        ViewNavigationSession session(QStringLiteral("all"));
        for (int i = 0; i < 130; ++i)
            QVERIFY(session.visit(chat(QStringLiteral("one"),
                                       QString::number(i))));
        int backward = 0;
        while (session.goBack())
            ++backward;
        QCOMPARE(backward, 100);
        QVERIFY(!session.canGoBack());
        QVERIFY(session.canGoForward());
        QCOMPARE(session.rowCount(), 0);
    }
};

QTEST_APPLESS_MAIN(ViewNavigationSessionTest)
#include "ViewNavigationSessionTest.moc"
