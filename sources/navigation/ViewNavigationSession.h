#pragma once

#include <optional>
#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include "NavigationTabsModel.h"

namespace Mattermost {

// UI-neutral semantic state of one workspace view. Backends and ChatAreas
// belong to accounts/presentation, never to this lightweight session.
// A single view may contain tabs from multiple accounts.
class ViewNavigationSession final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString viewId READ viewId CONSTANT)
    Q_PROPERTY(int activeTab READ activeTab NOTIFY activeTabChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
public:
    struct Location {
        enum class Kind { Chat, Saved, Drafts, RecentMentions, Search };
        Kind kind = Kind::Chat;
        QString accountId;
        QString channelId;
        QString rootId;
        QString postId;
        std::optional<NavigationTabsModel::ViewportAnchor> viewportAnchor;

        bool valid() const {
            return !accountId.isEmpty()
                && (kind != Kind::Chat || !channelId.isEmpty());
        }
        bool sameDestination(const Location& other) const {
            return kind == other.kind && accountId == other.accountId
                && (kind != Kind::Chat
                    || (channelId == other.channelId && rootId == other.rootId));
        }
    };
    // UI-independent transfer contract. Widgets may exchange their semantic
    // presentation state when switching views without transferring QWidgets.
    // A snapshot contains no Backend, Storage, credential or QObject pointer.
    struct Snapshot {
        QVector<NavigationTabsModel::Entry> tabs;
        int activeTab = -1;
        Location current;
        QVector<Location> back;
        QVector<Location> forward;
        // The independent right-hand thread is not a central tab.
        Location dock;
        bool dockVisible = false;
    };

    enum Role {
        AccountIdRole = Qt::UserRole + 1, ChannelIdRole,
        RootIdRole, PostIdRole, TitleRole, PinnedRole, ActiveRole
    };
    Q_ENUM(Role)

    explicit ViewNavigationSession(QString viewId, QObject* parent = nullptr);
    QString viewId() const { return _viewId; }
    int activeTab() const { return _activeTab; }
    bool canGoBack() const { return !_back.isEmpty(); }
    bool canGoForward() const { return !_forward.isEmpty(); }
    const Location& currentLocation() const { return _current; }
    const NavigationTabsModel& tabs() const { return _tabs; }
    Snapshot snapshot() const;
    // Reject malformed or ambiguous snapshots without changing state. The
    // existing list model is updated incrementally, never reset/recreated.
    bool restoreSnapshot(const Snapshot& snapshot);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The host handles actual presentation. This class never performs network
    // work, creates widgets or starts scrolling.
    int openTab(const Location& location, const QString& title = {},
                bool pinned = false);
    // Ordinary channel navigation reuses a non-pinned channel slot; explicit
    // open-in-new-tab uses openTab() instead. Account identity is never lost.
    int reuseOrOpenChannelTab(const Location& location, const QString& title = {});
    bool activateTab(int index);
    bool moveTab(int from, int to);
    bool closeTab(int index);
    bool setPinned(int index, bool pinned);
    bool updateBookmark(int index, const QString& postId,
                        std::optional<NavigationTabsModel::ViewportAnchor> anchor);
    bool visit(const Location& location);
    // Record a semantic visit without changing the central tab selection.
    // Docked threads share the same history while retaining the channel tab.
    bool recordVisit(const Location& location);
    // Update a passive viewport bookmark without creating a history step.
    // Reject a different destination, which must go through recordVisit().
    bool updateCurrentBookmark(const Location& location);
    std::optional<Location> goBack();
    std::optional<Location> goForward();

signals:
    void activeTabChanged(int index);
    void historyChanged();

private:
    static void pushHistory(QVector<Location>& history, const Location& location);
    void recordLocation(const Location& location);
    void chooseTab(int index);

    QString _viewId;
    NavigationTabsModel _tabs;
    int _activeTab = -1;
    Location _current;
    QVector<Location> _back;
    QVector<Location> _forward;
    Location _dock;
    bool _dockVisible = false;
};

} // namespace Mattermost
