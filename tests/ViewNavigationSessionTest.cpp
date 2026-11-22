#include <QtTest>
#include "navigation/ViewNavigationSession.h"

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
        QCOMPARE(from.openTab(alpha, QStringLiteral("Alpha"), true), 0);
        QCOMPARE(from.openTab(beta, QStringLiteral("Beta")), 1);
        QVERIFY(from.visit(other));
        auto transfer = from.snapshot();
        QCOMPARE(transfer.activeTab, -1);
        QCOMPARE(transfer.tabs.size(), 2);
        QCOMPARE(transfer.back.size(), 2);

        ViewNavigationSession target(QStringLiteral("view-b"));
        QCOMPARE(target.openTab(beta, QStringLiteral("Old")), 0);
        QCOMPARE(target.openTab(other, QStringLiteral("Remove")), 1);
        QSignalSpy reset(&target, &QAbstractItemModel::modelReset);
        QSignalSpy moved(&target, &QAbstractItemModel::rowsMoved);
        QSignalSpy inserted(&target, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(&target, &QAbstractItemModel::rowsRemoved);
        QVERIFY(target.restoreSnapshot(transfer));
        QCOMPARE(target.viewId(), QStringLiteral("view-b"));
        QCOMPARE(target.rowCount(), 2);
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
