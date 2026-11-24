#pragma once

#include <QObject>
#include <optional>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "NavigationTabsModel.h"
#include "ViewNavigationSession.h"

class QEvent;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QTimer;
class QWidget;

namespace Mattermost {

class Backend;
class ChatArea;
class ChannelTree;
class MainWindow;
class PostCollectionView;

/**
 * Main-window presentation state that is orthogonal to Mattermost transport:
 * browser-like semantic navigation history and the docked/right-hand thread
 * surface. Kept outside MainWindow so every navigation producer uses the same
 * presentation policy.
 */
class NavigationUiController final : public QObject
{
    Q_OBJECT
public:
    using Location = ViewNavigationSession::Location;

    static NavigationUiController& instance(MainWindow& window);

    explicit NavigationUiController(MainWindow& window);
    ~NavigationUiController() override;

    ChatArea* findThread(const QString& channelId, const QString& rootId) const;
    ChatArea* findThreadForBackend(const Backend& owner, const QString& channelId,
                                   const QString& rootId) const;
    /** Whether the exact conversation is visible in a focused window. */
    bool isConversationVisible(const QString& channelId,
                               const QString& rootId) const;
    void presentChannel(ChatArea* area);
    void presentThread(ChatArea* area);
    /** Choose the parent channel tab for an ordinary contextual thread click. */
    void selectContextualThreadTab(Backend& source, const QString& channelId,
                                   const QString& rootId);
    /** Thread-only tabs and detached windows must stay independent. */
    bool hasIndependentThreadPresentation(Backend& source, const QString& channelId,
                                          const QString& rootId) const;
    /** Whether ordinary thread navigation should first switch the central channel. */
    bool shouldOpenParentChannelForThread(Backend& source, const QString& channelId) const;
    void openInTab(Backend& source, const QString& channelId,
                   const QString& rootId,
                   const QString& postId,
                   const QStringList& contextPostIds,
                   bool reachedOldest,
                   bool reachedNewest);
    bool activateExistingTab(Backend& source, const QString& channelId,
                             const QString& rootId = QString(),
                             bool restoreBookmark = true);
    void saveSession();
    void restoreSession();
    // Called from MainWindow's destructor while all Widget descendants are
    // still valid. Flush once, then stop reacting to their destruction.
    void beginShutdown();
    void attachBackend(Backend& source);
    void cancelOtherNavigationRequests(const Backend* source);
    // Empty accountId binds exact account-qualified View state. The explicit
    // alias is retained for the transitional one-account presenters/fixtures.
    void bindViewSession(ViewNavigationSession* session, const QString& accountId = {});
    // Stop writing presentation state into an inactive view.
    void unbindViewSession();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupMainWindow();
    QString sessionSettingsKey() const;
    void scheduleSessionSave();
    void syncViewSession();
    void restoreBoundViewSession(bool adoptExistingPresentation);
    void showEmptyConversation();
    bool usesSharedView() const;
    ViewNavigationSession* historySession() const;
    ViewNavigationSession::Location semanticLocation(const Location& location) const;
    bool presentationLocation(const ViewNavigationSession::Location& source,
                              Location& result) const;
    void trackSessionArea(ChatArea* area);
    void restoreSessionBookmark(
        ChatArea* area, const QString& postId,
        std::optional<NavigationTabsModel::ViewportAnchor> anchor = std::nullopt);
    void setupSidebarHeader();
    void setupThreadPane();
    void setupTabShortcuts();
    void closeThreadDockIfDifferent(const QString& accountId, const QString& channelId,
                                    bool force = false);
    ChatArea* centralTabArea() const;
    void closeActiveTab();
    void activatePreviousTab();
    void activateNextTab();
    void updateHistoryButtons();

    Location captureLocation(ChatArea* area) const;
    Location captureCollection(PostCollectionView* page) const;
    void recordArea(ChatArea* area);
    void recordCollection(PostCollectionView* page);
    void recordLocation(const Location& location, ChatArea* area);
    void navigateTo(const Location& location);
    PostCollectionView* findCollection(const Location& location) const;

    NavigationTabsModel::Entry tabEntry(const Location& location) const;
    Location tabLocation(const NavigationTabsModel::Entry& entry) const;
    QString tabTitle(const Location& location) const;
    int appendNavigationTab(const Location& location,
                            bool deferVisibility = false);
    void ensureInitialTab();
    void refreshTabBarVisibility();
    void setNavigationTabTitle(int index, const QString& title);
    void refreshTabUnreadVisual(const QString& accountId, const QString& channelId);
    void refreshAllTabUnreadVisuals(const QString& accountId);
    void updateTab(int index, const Location& location);
    void saveActiveTabLocation();
    void activateTab(int index, bool restoreBookmark = true);
    void closeTab(int index);
    void removeTab(int index, bool closeThread);
    int tabIndexForThread(ChatArea* area) const;
    int firstChannelTab() const;
    int releaseThreadFromTabSurface(ChatArea* area);
    void tabifyThread(ChatArea* area, int tabIndex);
    void goBack();
    void goForward();

    void ensureTabPinAction(ChatArea* area);
    void updateTabPinAction(ChatArea* area);
    void ensureThreadButton(ChatArea* area);
    void attachThread(ChatArea* area);
    void detachThread(ChatArea* area);
    void updateThreadButton(ChatArea* area);
    void syncSplitterEdgeGutters();
    void rememberThreadPaneWidth();
    void persistThreadPaneWidth();

    ChatArea* findThreadForEntry(const NavigationTabsModel::Entry& entry) const;
    ChatArea* currentChannelPage() const;
    Backend* backend() const;
    Backend* backendForAccount(const QString& accountId) const;
    // Transitional single-account presenter. Resolve stable identity from the
    // owning Account, never from the selected View or a server channel ID.
    QString presentationAccountId() const;
    static QString accountIdForBackend(const Backend& backend);

    MainWindow& window;
    ChannelTree* channelTree = nullptr;
    QStackedWidget* mainStack = nullptr;
    QPointer<QWidget> emptyConversation;
    QSplitter* sidebarSplitter = nullptr;
    QPointer<QSplitter> contentSplitter;
    QWidget* contentHost = nullptr;
    QStackedWidget* navigationSurfaceStack = nullptr;
    QTabBar* navigationTabs = nullptr;
    QStackedWidget* threadStack = nullptr;
    QToolButton* backButton = nullptr;
    QToolButton* forwardButton = nullptr;

    QSet<Backend*> attachedBackends;
    QPointer<ChatArea> activeArea;
    Location currentLocation;
    // The QtCore session owns Back/Forward; no second Widgets history stacks.
    ViewNavigationSession* startupHistory = nullptr;
    NavigationTabsModel tabModel;
    int activeTabIndex = -1;
    bool switchingTabs = false;
    bool replayingHistory = false;
    // An asynchronous Back/Forward thread restore may first activate its
    // parent conversation. Suppress that intermediate channel history entry.
    std::optional<Location> pendingHistoryThread;
    // Ordinary contextual thread navigation can also activate/admit the parent
    // channel first. Its intermediate presentation must not be a Back step.
    std::optional<Location> pendingContextualThread;
    bool threadSplitterStateRestored = false;
    int _preferredThreadPaneWidth = 0;
    QByteArray _legacyThreadSplitterState;
    QTimer* sessionSaveTimer = nullptr;
    bool sessionRestored = false;
    bool shuttingDown = false;
    bool restoringSession = false;
    bool applyingViewSession = false;
    QPointer<ViewNavigationSession> viewNavigationSession;
    QString viewNavigationAccountId;
    bool everBoundView = false;
};

} // namespace Mattermost