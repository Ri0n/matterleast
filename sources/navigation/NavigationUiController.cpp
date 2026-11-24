#include "NavigationUiController.h"

#include <algorithm>
#include <cmath>

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/Account.h"
#include "backend/FollowingModel.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "mainwindow.h"
#include "options/MLOptions.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ChannelPageHost.h"
#include "navigation/ViewNavigationSession.h"
#include "navigation/ThreadPaneLayout.h"
#include "navigation/ThreadSidebarContextPolicy.h"
#include "Settings.h"
#include "post-collection/PostCollectionView.h"
#include "ui/IconUtils.h"
#include "ui/ThinSplitter.h"

namespace Mattermost {
namespace {

QIcon threadPresentationIcon(const QPalette& palette)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QPen pen(palette.color(QPalette::ButtonText));
    pen.setWidth(1);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Deliberately use the literal "two overlapping squares" detach glyph
    // rather than SP_TitleBarNormalButton: desktop styles are free to render
    // that standard pixmap as a platform-specific window-management symbol.
    painter.drawRect(QRect(2, 5, 9, 8));
    painter.drawRect(QRect(5, 2, 9, 8));
    return QIcon(pixmap);
}

QIcon threadTabIcon(const QPalette& palette)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QPen pen(palette.color(QPalette::ButtonText));
    pen.setWidth(1);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRect(2, 3, 12, 10));
    painter.drawLine(QPoint(5, 6), QPoint(11, 6));
    painter.drawLine(QPoint(8, 3), QPoint(8, 9));
    return QIcon(pixmap);
}

} // namespace

NavigationUiController& NavigationUiController::instance(MainWindow& window)
{
    if (auto* existing = window.findChild<NavigationUiController*>(
            QStringLiteral("navigationUiController"), Qt::FindDirectChildrenOnly)) {
        return *existing;
    }
    return *new NavigationUiController(window);
}

NavigationUiController::NavigationUiController(MainWindow& mainWindow)
    : QObject(&mainWindow)
    , window(mainWindow)
{
    setObjectName(QStringLiteral("navigationUiController"));
    // Own one transient history during initial window construction. As soon as
    // the account view is known, AccountManager's session takes over; neither
    // path maintains a second Back/Forward stack in Widgets.
    startupHistory = new ViewNavigationSession(QStringLiteral("startup"), this);
    sessionSaveTimer = new QTimer(this);
    sessionSaveTimer->setSingleShot(true);
    sessionSaveTimer->setInterval(500);
    connect(sessionSaveTimer, &QTimer::timeout, this, &NavigationUiController::saveSession);
    setupMainWindow();
    qApp->installEventFilter(this);
}

NavigationUiController::~NavigationUiController()
{
    // MainWindow::beginShutdown() must run before QObject recursively destroys
    // the sidebar/thread stacks. Do not dereference Widget members here: they
    // may already have been deleted by QObjectPrivate::deleteChildren().
    if (qApp)
        qApp->removeEventFilter(this);
}

void NavigationUiController::beginShutdown()
{
    if (shuttingDown)
        return;
    // Saving navigation belongs to MainWindow::saveState (aboutToQuit or
    // prepareForLogout), not QObject teardown. A detached ChatArea may have
    // closed after an explicit snapshot was saved; rewriting that snapshot
    // as its test fixture is destroyed would lose the user's last state.
    // Stop observing children before QWidget teardown, but do not persist
    // an incomplete or partially disposed presentation.
    persistThreadPaneWidth();
    shuttingDown = true;
    if (sessionSaveTimer)
        sessionSaveTimer->stop();
    if (qApp)
        qApp->removeEventFilter(this);
}

void NavigationUiController::setupMainWindow()
{
    channelTree = window.findChild<ChannelTree*>(QStringLiteral("channelList"));
    mainStack = window.findChild<QStackedWidget*>(QStringLiteral("chatAreaStackedWidget"));

    setupSidebarHeader();
    setupThreadPane();

    auto* contextualOption = MLOptions::instance()->optionObject<bool>(
        THREAD_SIDEBAR_FOLLOWS_CHANNEL, THREAD_SIDEBAR_FOLLOWS_CHANNEL_DEFAULT);
    connect(contextualOption, &MLOptionObject::changed, this,
            [this](const QVariant& value) {
        if (!value.toBool() || !mainStack || !navigationSurfaceStack) return;
        QWidget* current = navigationSurfaceStack->currentWidget();
        ChatArea* active = current == mainStack
            ? qobject_cast<ChatArea*>(mainStack->currentWidget())
            : qobject_cast<ChatArea*>(current);
        if (active) {
            closeThreadDockIfDifferent(accountIdForBackend(active->getBackend()),
                                       active->getChannel().id);
        }
    });

    if (Backend* sourceBackend = backend())
        attachBackend(*sourceBackend);

    if (mainStack) {
        connect(mainStack, &QStackedWidget::currentChanged, this,
                [this](int index) {
            syncSplitterEdgeGutters();

            QWidget* page = mainStack->widget(index);
            if (auto* channelArea = qobject_cast<ChatArea*>(page);
                channelArea && !channelArea->isThread) {
                closeThreadDockIfDifferent(accountIdForBackend(channelArea->getBackend()),
                                       channelArea->getChannel().id);
            }
            const bool chatSurface = qobject_cast<ChatArea*>(page) != nullptr;
            if (!chatSurface) {
                if (auto* collection = qobject_cast<PostCollectionView*>(page)) {
                    recordCollection(collection);
                }
                // Saved/Drafts/Search are central surfaces just like channels.
                // They replace only the central tab surface; a docked thread on
                // the right remains an independent presentation pane.
                if (!switchingTabs && navigationSurfaceStack && mainStack) {
                    navigationSurfaceStack->setCurrentWidget(mainStack);
                }
                if (navigationTabs) {
                    navigationTabs->hide();
                }
                return;
            }

            refreshTabBarVisibility();
        });
    }

    if (mainStack) {
        connect(&ChannelPageHost::instance(*mainStack), &ChannelPageHost::channelActivated,
                this, [this] {
            if (switchingTabs || restoringSession) return;
            QTimer::singleShot(0, this, [this] {
                // A deferred channel activation must not steal an independently
                // activated thread tab (including startup session replay).
                if (mainStack && (!navigationSurfaceStack
                    || navigationSurfaceStack->currentWidget() == mainStack)) {
                    recordArea(currentChannelPage());
                }
            });
        });
        recordArea(currentChannelPage());
    }
    if (channelTree) {
        channelTree->installEventFilter(this);
        if (channelTree->viewport()) {
            channelTree->viewport()->installEventFilter(this);
        }
    }

    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        saveSession();
        persistThreadPaneWidth();
    });

    updateHistoryButtons();
}

void NavigationUiController::setupSidebarHeader()
{
    auto* header = window.findChild<QWidget*>(QStringLiteral("lefttop_frame"));
    auto* searchButton = window.findChild<QToolButton*>(QStringLiteral("searchButton"));
    auto* username = window.findChild<QLabel*>(QStringLiteral("usernameLabel"));
    if (username) {
        username->hide();
    }
    // PresenceStatusLabel already hides plain availability (the avatar has a
    // badge). Keep custom status visible here, including emoji-only content.
    if (!header || !searchButton) {
        return;
    }

    auto* layout = header->findChild<QHBoxLayout*>(QStringLiteral("horizontalLayout"));
    if (!layout) {
        return;
    }

    backButton = new QToolButton(header);
    backButton->setObjectName(QStringLiteral("navigationBackButton"));
    backButton->setAutoRaise(true);
    backButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    backButton->setIcon(window.style()->standardIcon(QStyle::SP_ArrowBack));
    backButton->setToolTip(tr("Back"));
    backButton->setAccessibleName(tr("Back"));

    forwardButton = new QToolButton(header);
    forwardButton->setObjectName(QStringLiteral("navigationForwardButton"));
    forwardButton->setAutoRaise(true);
    forwardButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    forwardButton->setIcon(window.style()->standardIcon(QStyle::SP_ArrowForward));
    forwardButton->setToolTip(tr("Forward"));
    forwardButton->setAccessibleName(tr("Forward"));

    int searchIndex = layout->indexOf(searchButton);
    if (searchIndex < 0) {
        searchIndex = layout->count();
    }
    layout->insertWidget(searchIndex, backButton);
    layout->insertWidget(searchIndex + 1, forwardButton);

    connect(backButton, &QToolButton::clicked, this, &NavigationUiController::goBack);
    connect(forwardButton, &QToolButton::clicked, this, &NavigationUiController::goForward);

    auto* backShortcut = new QShortcut(QKeySequence(QKeySequence::Back), &window);
    auto* forwardShortcut = new QShortcut(QKeySequence(QKeySequence::Forward), &window);
    connect(backShortcut, &QShortcut::activated, this, &NavigationUiController::goBack);
    connect(forwardShortcut, &QShortcut::activated, this, &NavigationUiController::goForward);
}

void NavigationUiController::setupThreadPane()
{
    if (!mainStack) {
        return;
    }

    const auto splitters = window.findChildren<QSplitter*>();
    for (QSplitter* candidate : splitters) {
        if (candidate && candidate->indexOf(mainStack) >= 0) {
            sidebarSplitter = candidate;
            break;
        }
    }
    if (!sidebarSplitter) {
        return;
    }
    sidebarSplitter->setObjectName(QStringLiteral("sidebarSplitter"));

    const int oldIndex = sidebarSplitter->indexOf(mainStack);
    const QList<int> outerSizes = sidebarSplitter->sizes();

    // Build the replacement pane parentless. MainWindow is already visible when
    // this controller is bootstrapped; constructing a QWidget directly under a
    // visible QSplitter leaves the new subtree explicitly hidden while mainStack
    // is reparented into it. insertWidget() below becomes the single ownership
    // transition, matching the previously working splitter setup.
    contentHost = new QWidget;
    contentHost->setObjectName(QStringLiteral("navigationContentHost"));
    auto* hostLayout = new QVBoxLayout(contentHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);

    navigationTabs = new QTabBar(contentHost);
    navigationTabs->setObjectName(QStringLiteral("navigationTabs"));
    navigationTabs->setDocumentMode(true);
    navigationTabs->setMovable(true);
    navigationTabs->setTabsClosable(true);
    navigationTabs->setExpanding(false);
    navigationTabs->setUsesScrollButtons(true);
    navigationTabs->setElideMode(Qt::ElideRight);
    navigationTabs->installEventFilter(this);
    navigationTabs->hide();
    hostLayout->addWidget(navigationTabs);

    // Central navigation and the right thread pane are independent siblings.
    // A channel or thread tab replaces only the left/central surface; it must
    // never hide a docked thread that happens to be open at the same time.
    contentSplitter = new ThinSplitter(Qt::Horizontal);
    contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
    contentSplitter->setChildrenCollapsible(true);
    contentSplitter->setOpaqueResize(true);
    hostLayout->addWidget(contentSplitter, 1);

    navigationSurfaceStack = new QStackedWidget(contentSplitter);
    navigationSurfaceStack->setObjectName(QStringLiteral("navigationSurfaceStack"));
    navigationSurfaceStack->addWidget(mainStack);
    navigationSurfaceStack->setCurrentWidget(mainStack);
    contentSplitter->addWidget(navigationSurfaceStack);

    threadStack = new QStackedWidget(contentSplitter);
    threadStack->setObjectName(QStringLiteral("threadStack"));
    threadStack->setMinimumWidth(280);
    threadStack->hide();
    contentSplitter->addWidget(threadStack);
    contentSplitter->setStretchFactor(0, 2);
    contentSplitter->setStretchFactor(1, 1);

    sidebarSplitter->insertWidget(std::max(0, oldIndex), contentHost);
    if (!outerSizes.isEmpty()) {
        sidebarSplitter->setSizes(outerSizes);
    }

    // Reparenting a visible widget hides it. Establish the central surface and
    // splitter explicitly; thread presentation controls only the right sibling.
    contentHost->show();
    contentSplitter->show();
    navigationSurfaceStack->show();
    mainStack->show();

    // This navigation controller replaces a QSplitter child after the
    // MainWindow constructor's restoreState() has run. Reapply the saved
    // left width now that the central widget hierarchy is final.
    window.restoreSidebarWidth();

    // Restore the right-pane width only after the thread is visible.
    // Restoring QSplitter state here would use a hidden child with width zero.
    _preferredThreadPaneWidth = MLOptions::instance()->value<int>(
        QStringLiteral("content_thread_width"), 0);
    _legacyThreadSplitterState = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("content_splitter_state"));
    connect(contentSplitter, &QSplitter::splitterMoved, this,
            [this] {
        rememberThreadPaneWidth();
    });
    // QSplitter persists handle width in its state; do not let legacy 4 px
    // states reintroduce layout space between channel and thread panes.
    contentSplitter->setHandleWidth(ThinSplitter::VisibleHandleExtent);

    connect(navigationTabs, &QTabBar::currentChanged, this,
            [this](int index) {
        if (!switchingTabs && index >= 0) {
            activateTab(index);
        }
    });
    connect(navigationTabs, &QTabBar::tabCloseRequested,
            this, &NavigationUiController::closeTab);
    connect(navigationTabs, &QTabBar::tabMoved, this,
            [this](int from, int to) {
        tabModel.move(from, to);
        if (activeTabIndex == from) {
            activeTabIndex = to;
        } else if (from < activeTabIndex && activeTabIndex <= to) {
            --activeTabIndex;
        } else if (to <= activeTabIndex && activeTabIndex < from) {
            ++activeTabIndex;
        }
        scheduleSessionSave();
    });
    setupTabShortcuts();
}

void NavigationUiController::setupTabShortcuts()
{
    if (!navigationTabs) return;
    auto* close = new QShortcut(QKeySequence(QStringLiteral("Ctrl+W")), &window);
    auto* previous = new QShortcut(QKeySequence(QStringLiteral("Ctrl+PgUp")), &window);
    auto* next = new QShortcut(QKeySequence(QStringLiteral("Ctrl+PgDn")), &window);
    connect(close, &QShortcut::activated, this, &NavigationUiController::closeActiveTab);
    connect(previous, &QShortcut::activated, this, &NavigationUiController::activatePreviousTab);
    connect(next, &QShortcut::activated, this, &NavigationUiController::activateNextTab);
}

ChatArea* NavigationUiController::centralTabArea() const
{
    if (!navigationSurfaceStack) return nullptr;
    QWidget* surface = navigationSurfaceStack->currentWidget();
    if (surface == mainStack) {
        return mainStack ? qobject_cast<ChatArea*>(mainStack->currentWidget()) : nullptr;
    }
    return qobject_cast<ChatArea*>(surface);
}

void NavigationUiController::closeActiveTab()
{
    if (navigationTabs && tabModel.count() > 1 && activeTabIndex >= 0
        && centralTabArea()) {
        closeTab(activeTabIndex);
    }
}

void NavigationUiController::activatePreviousTab()
{
    if (!navigationTabs || tabModel.count() <= 1) return;
    const int index = tabModel.cycledIndex(activeTabIndex, -1);
    if (index >= 0 && index != activeTabIndex) activateTab(index);
}

void NavigationUiController::activateNextTab()
{
    if (!navigationTabs || tabModel.count() <= 1) return;
    const int index = tabModel.cycledIndex(activeTabIndex, 1);
    if (index >= 0 && index != activeTabIndex) activateTab(index);
}

bool NavigationUiController::canPresentBackend(const Backend& source) const
{
    return backendForAccount(accountIdForBackend(source)) == &source;
}

QVector<Backend*> NavigationUiController::visibleBackends() const
{
    QVector<Backend*> result;
    for (auto* owner : attachedBackends)
        if (canPresentBackend(*owner)) result.append(owner);
    return result;
}

void NavigationUiController::cancelOtherNavigationRequests(const Backend* source)
{
    for (Backend* owner : attachedBackends) {
        if (owner != source) {
            if (auto* service = owner->findChild<AppNavigationService*>(
                    QStringLiteral("appNavigationService"), Qt::FindDirectChildrenOnly))
                service->cancelPendingNavigation();
        }
    }
}

void NavigationUiController::refreshAttachedDestination(Backend& source)
{
    if (!usesSharedView() || !canPresentBackend(source)) return;
    refreshAllTabUnreadVisuals(accountIdForBackend(source));
    if (activeArea || currentLocation.accountId != accountIdForBackend(source))
        return;
    // Restore only the retained destination of the arriving account. Do not
    // reopen another account's current chat on each connection or reconnect.
    const auto location = currentLocation;
    if (location.isChat() && source.getStorage().getChannelById(location.channelId)) {
        const int index = tabModel.findDestination(location.channelId, location.rootId,
                                                   location.accountId);
        if (index >= 0) activateTab(index);
        else navigateTo(location);
    }
}

void NavigationUiController::detachBackend(Backend& source)
{
    if (!attachedBackends.contains(&source)) return;
    saveActiveTabLocation();
    syncViewSession();
    const QString accountId = accountIdForBackend(source);
    const QScopedValueRollback<bool> applying(applyingViewSession, true);
    const QScopedValueRollback<bool> switching(switchingTabs, true);
    attachedBackends.remove(&source);
    AppNavigationService::instance(source).unbindMainWindow(window);
    disconnect(&source, nullptr, this, nullptr);
    disconnect(&SidebarService::instance(source), nullptr, this, nullptr);
    disconnect(&FollowingModel::instance(source), nullptr, this, nullptr);
    disconnect(&AppNavigationService::instance(source), nullptr, this, nullptr);
    if (pendingHistoryThread && pendingHistoryThread->accountId == accountId)
        pendingHistoryThread.reset();
    if (pendingContextualThread && pendingContextualThread->accountId == accountId)
        pendingContextualThread.reset();

    // Threads may already be hidden, removed from a stack and awaiting
    // DeferredDelete, or detached top-level windows. Release those before
    // their parent channel; all of them still reference the old Storage.
    QVector<QPointer<ChatArea>> threads;
    for (auto* widget : QApplication::allWidgets()) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (area && area->isThread && &area->getBackend() == &source)
            threads.append(area);
    }
    if (currentLocation.accountId == accountId)
        showEmptyConversation();
    for (const auto& thread : threads) {
        if (!thread) continue;
        thread->onDeactivate();
        if (navigationSurfaceStack) navigationSurfaceStack->removeWidget(thread);
        if (threadStack) threadStack->removeWidget(thread);
        delete thread.data();
    }
    if (threadStack && !threadStack->count()) threadStack->hide();

    if (mainStack) {
        for (int i = mainStack->count() - 1; i >= 0; --i) {
            QWidget* page = mainStack->widget(i);
            auto* area = qobject_cast<ChatArea*>(page);
            auto* collection = qobject_cast<PostCollectionView*>(page);
            if ((!area || &area->getBackend() != &source)
                && (!collection || &collection->backendInstance() != &source))
                continue;
            if (area) area->onDeactivate();
            mainStack->removeWidget(page);
            delete page;
        }
    }
    // The View retains its semantic destinations. They can become available
    // again with a new Backend, but no tab retains a pointer to old Storage.
    syncSplitterEdgeGutters();
}

void NavigationUiController::attachBackend(Backend& source)
{
    if (attachedBackends.contains(&source)) return;
    attachedBackends.insert(&source);
    connect(&source, &QObject::destroyed, this, [this, key = &source] {
        attachedBackends.remove(key);
    });
    Backend* sourceBackend = &source;
    // A new explicit navigation cancels a deferred Back/Forward replay.
    // The history path below calls MainWindow directly, so it does not
    // publish this semantic-navigation signal itself.
    connect(&AppNavigationService::instance(*sourceBackend),
            &AppNavigationService::navigationStarted, this,
            [this, sourceBackend] {
                if (!canPresentBackend(*sourceBackend)) return;
                pendingHistoryThread.reset();
                pendingContextualThread.reset();
            });
    auto& sidebar = SidebarService::instance(*sourceBackend);
    const QString accountId = accountIdForBackend(*sourceBackend);
    QPointer<Backend> sourceGuard(sourceBackend);
    const auto refreshAll = [this, sourceGuard, accountId] {
        if (sourceGuard && backendForAccount(accountId) == sourceGuard.data())
            refreshAllTabUnreadVisuals(accountId);
    };
    connect(&sidebar, &SidebarService::channelActivityChanged, this,
            [this, sourceGuard, accountId](const QString& channelId) {
        if (sourceGuard && backendForAccount(accountId) == sourceGuard.data())
            refreshTabUnreadVisual(accountId, channelId);
    });
    connect(&sidebar, &SidebarService::channelActivityReset, this, refreshAll);
    connect(&FollowingModel::instance(*sourceBackend), &FollowingModel::changed,
            this, refreshAll);
}

ChatArea* NavigationUiController::currentChannelPage() const
{
    return mainStack ? qobject_cast<ChatArea*>(mainStack->currentWidget()) : nullptr;
}

Backend* NavigationUiController::backend() const
{
    return window.backendOrNull();
}

Backend* NavigationUiController::backendForAccount(const QString& accountId) const
{
    if (usesSharedView() && !viewNavigationSession->allowsAccount(accountId))
        return nullptr;
    auto* owner = window.navigationBackend(accountId);
    return owner && attachedBackends.contains(owner) ? owner : nullptr;
}

QString NavigationUiController::accountIdForBackend(const Backend& source)
{
    const auto* account = qobject_cast<const Account*>(source.parent());
    // Standalone widget fixtures do not have an AccountManager. Their
    // private history still uses a valid, isolated semantic account key.
    return account ? account->id() : QStringLiteral("_startup");
}

QString NavigationUiController::presentationAccountId() const
{
    const auto* owner = backend();
    return owner ? accountIdForBackend(*owner) : QString();
}

NavigationUiController::Location
NavigationUiController::captureLocation(ChatArea* area) const
{
    Location location;
    if (!area) {
        return location;
    }

    location.accountId = accountIdForBackend(area->getBackend());
    location.channelId = area->getChannel().id;
    if (area->isThread) {
        location.rootId = area->root_id;
    }
    const QString pendingBookmark = area->property("sessionBookmark").toString();
    if (!pendingBookmark.isEmpty()) {
        location.postId = pendingBookmark;
        // A detached/docked thread has no tab entry. Its still-pending
        // bookmark is stored on the presenting ChatArea until materialized.
        const QVariantMap pending = area->property("sessionBookmarkAnchor").toMap();
        if (pending.value(QStringLiteral("post")).toString() == pendingBookmark
            && std::isfinite(pending.value(QStringLiteral("fraction")).toDouble())) {
            location.viewportAnchor = NavigationTabsModel::ViewportAnchor {
                pendingBookmark,
                pending.value(QStringLiteral("bottom")).toBool(),
                pending.value(QStringLiteral("fraction")).toDouble()};
            return location;
        }
        // The newly constructed ChatArea is not yet a reliable viewport
        // geometry source. Reuse the saved tab anchor while the post is being
        // fetched, rather than accidentally replacing it with the initial end.
        if (const auto* tab = tabModel.at(
                tabModel.findDestination(location.channelId, location.rootId, location.accountId));
            tab && tab->viewportAnchor
                && tab->viewportAnchor->postId == pendingBookmark) {
            location.viewportAnchor = tab->viewportAnchor;
        } else if (currentLocation.sameDestination(location)
                   && currentLocation.viewportAnchor
                   && currentLocation.viewportAnchor->postId == pendingBookmark) {
            location.viewportAnchor = currentLocation.viewportAnchor;
        }
        return location;
    }

    if (auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"))) {
        ChatLogWidget::SemanticViewportBookmark bookmark;
        if (log->captureSemanticViewportBookmark(bookmark)) {
            location.postId = bookmark.postId;
            location.viewportAnchor = NavigationTabsModel::ViewportAnchor {
                bookmark.postId,
                bookmark.point == LongListWidget::SemanticAnchorPoint::ItemBottom,
                bookmark.fraction};
            return location;
        }
        QString postId;
        if (log->captureViewportBookmark(postId)) {
            location.postId = postId;
            return location;
        }
    }

    location.postId = area->storedNavigationBookmark();
    return location;
}

NavigationUiController::Location
NavigationUiController::captureCollection(PostCollectionView* page) const
{
    Location location;
    if (!page) {
        return location;
    }

    location.accountId = accountIdForBackend(page->backendInstance());
    switch (page->viewMode()) {
    case PostCollectionView::Mode::Saved:
        location.kind = Location::Kind::Saved;
        break;
    case PostCollectionView::Mode::Drafts:
        location.kind = Location::Kind::Drafts;
        break;
    case PostCollectionView::Mode::RecentMentions:
        location.kind = Location::Kind::RecentMentions;
        break;
    case PostCollectionView::Mode::Search:
        location.kind = Location::Kind::Search;
        break;
    case PostCollectionView::Mode::Pinned:
        break;
    }
    return location;
}

PostCollectionView* NavigationUiController::findCollection(const Location& location) const
{
    if (!mainStack || location.isChat()) {
        return nullptr;
    }

    for (int index = 0; index < mainStack->count(); ++index) {
        auto* page = qobject_cast<PostCollectionView*>(mainStack->widget(index));
        if (page && captureCollection(page).sameDestination(location)) {
            return page;
        }
    }
    return nullptr;
}

NavigationTabsModel::Entry
NavigationUiController::tabEntry(const Location& location) const
{
    NavigationTabsModel::Entry entry;
    entry.accountId = location.accountId;
    entry.channelId = location.channelId;
    entry.rootId = location.rootId;
    entry.postId = location.postId;
    entry.viewportAnchor = location.viewportAnchor;
    entry.title = tabTitle(location);
    return entry;
}

NavigationUiController::Location
NavigationUiController::tabLocation(const NavigationTabsModel::Entry& entry) const
{
    Location location;
    location.accountId = entry.accountId;
    location.channelId = entry.channelId;
    location.rootId = entry.rootId;
    location.postId = entry.postId;
    location.viewportAnchor = entry.viewportAnchor;
    return location;
}

QString NavigationUiController::tabTitle(const Location& location) const
{
    QString title = location.channelId;
    if (Backend* sourceBackend = backendForAccount(location.accountId)) {
        if (BackendChannel* channel =
                sourceBackend->getStorage().getChannelById(location.channelId)) {
            title = channel->display_name.trimmed();
            if (title.isEmpty()) {
                title = channel->name.trimmed();
            }
            if (title.isEmpty()) {
                title = channel->id;
            }
        }
    }

    if (!location.rootId.isEmpty()) {
        title = tr("%1 · Thread").arg(title);
    }
    return title;
}

int NavigationUiController::appendNavigationTab(const Location& location,
                                              bool deferVisibility)
{
    if (!navigationTabs || !location.isChat() || !location.valid()) {
        return -1;
    }

    const int oldCount = tabModel.count();
    const int index = tabModel.append(tabEntry(location));
    if (index < 0) {
        return -1;
    }

    const auto* entry = tabModel.at(index);
    const QString title = entry ? entry->title : tabTitle(location);
    if (index >= oldCount) {
        navigationTabs->addTab(title);
    }
    setNavigationTabTitle(index, title);
    refreshTabUnreadVisual(location.accountId, location.channelId);
    if (!deferVisibility) {
        refreshTabBarVisibility();
    }
    return index;
}

void NavigationUiController::setNavigationTabTitle(
    int index, const QString& title)
{
    if (!navigationTabs || index < 0 || index >= navigationTabs->count()) {
        return;
    }

    navigationTabs->setTabText(index, title);
    navigationTabs->setTabToolTip(index, title);
}

void NavigationUiController::refreshTabUnreadVisual(const QString& accountId,
                                                    const QString& channelId)
{
    if (!navigationTabs || channelId.isEmpty()) {
        return;
    }

    Backend* sourceBackend = backendForAccount(accountId);
    BackendChannel* channel = sourceBackend
        ? sourceBackend->getStorage().getChannelById(channelId)
        : nullptr;
    if (!sourceBackend || !channel) {
        return;
    }

    auto& sidebar = SidebarService::instance(*sourceBackend);
    auto& following = FollowingModel::instance(*sourceBackend);
    const bool channelMarked =
        sidebar.isChannelUnread(*channel)
        || sidebar.hasUnreadMention(channelId);

    for (int index = 0; index < tabModel.count(); ++index) {
        const auto* entry = tabModel.at(index);
        if (!entry || entry->accountId != accountId
            || entry->channelId != channelId) {
            continue;
        }

        // A thread is an independent read domain. Reading it cannot consume
        // parent-channel activity, so its tab must use the same CRT/manual
        // attention state as its own Following row.
        const auto* thread = entry->rootId.isEmpty()
            ? nullptr : following.findEntry(channelId, entry->rootId);
        const bool marked = entry->rootId.isEmpty()
            ? channelMarked : thread && thread->requiresAttention();

        navigationTabs->setTabText(
            index,
            marked ? QStringLiteral("★ ") + entry->title : entry->title);
    }
}

void NavigationUiController::refreshAllTabUnreadVisuals(const QString& accountId)
{
    QSet<QString> channelIds;
    for (int index = 0; index < tabModel.count(); ++index) {
        if (const auto* entry = tabModel.at(index);
            entry && entry->accountId == accountId && !entry->channelId.isEmpty()) {
            channelIds.insert(entry->channelId);
        }
    }
    for (const QString& channelId : channelIds) {
        refreshTabUnreadVisual(accountId, channelId);
    }
}

void NavigationUiController::ensureInitialTab()
{
    if (!navigationTabs || !tabModel.isEmpty()) {
        return;
    }

    auto* area = mainStack
        ? qobject_cast<ChatArea*>(mainStack->currentWidget())
        : nullptr;
    const Location location = captureLocation(area);
    if (!location.valid()) {
        return;
    }

    const int index = appendNavigationTab(location);
    if (index < 0) {
        return;
    }

    activeTabIndex = index;
    switchingTabs = true;
    navigationTabs->setCurrentIndex(index);
    switchingTabs = false;
}

void NavigationUiController::refreshTabBarVisibility()
{
    if (navigationTabs) {
        navigationTabs->setVisible(tabModel.shouldShowTabBar());
    }
}

void NavigationUiController::updateTab(int index, const Location& location)
{
    if (!navigationTabs || !location.isChat() || !location.valid()
        || !tabModel.replace(index, tabEntry(location))) {
        return;
    }
    setNavigationTabTitle(index, tabTitle(location));
    refreshTabUnreadVisual(location.accountId, location.channelId);
}

void NavigationUiController::saveActiveTabLocation()
{
    const auto* entry = tabModel.at(activeTabIndex);
    if (!entry) {
        return;
    }

    ChatArea* area = nullptr;
    if (!entry->rootId.isEmpty()) {
        area = findThreadForEntry(*entry);
    } else if (!navigationSurfaceStack
               || navigationSurfaceStack->currentWidget() == mainStack) {
        area = mainStack
            ? qobject_cast<ChatArea*>(mainStack->currentWidget())
            : nullptr;
    }

    if (!area) {
        return;
    }

    const Location location = captureLocation(area);
    const Location expected = tabLocation(*entry);
    if (location.valid() && location.sameDestination(expected)) {
        // Capture before the outgoing tab's ChatArea is destroyed. History
        // must not recapture an already hidden or deactivated presentation.
        if (currentLocation.isChat() && currentLocation.sameDestination(location)) {
            currentLocation = location;
        }
        updateTab(activeTabIndex, location);
        // Both history and tabs use the exact same measured viewport snapshot.
        // Never resurrect the previous anchor after a new scroll that could
        // not yet produce a concrete post.
        if (location.viewportAnchor) {
            tabModel.setViewportAnchor(activeTabIndex, *location.viewportAnchor);
        } else if (auto* log = area->findChild<ChatLogWidget*>(
                       QStringLiteral("listWidget")); log && log->source()) {
            tabModel.clearViewportAnchor(activeTabIndex);
        }
    }
}

int NavigationUiController::firstChannelTab() const
{
    for (int i = 0; i < tabModel.count(); ++i) {
        const auto* entry = tabModel.at(i);
        if (entry && entry->rootId.isEmpty()) {
            return i;
        }
    }
    return -1;
}

int NavigationUiController::tabIndexForThread(ChatArea* area) const
{
    if (!area || !area->isThread) {
        return -1;
    }
    return tabModel.findDestination(area->getChannel().id, area->root_id,
                                    accountIdForBackend(area->getBackend()));
}

int NavigationUiController::releaseThreadFromTabSurface(ChatArea* area)
{
    if (!area || !area->isThread
        || !area->property("threadTabbed").toBool()) {
        return -1;
    }

    const int tabIndex = tabIndexForThread(area);
    if (tabIndex < 0) {
        return -1;
    }

    // Presentation transitions own QWidget reparenting. Remove the thread from
    // the tab surface first and clear the tabbed marker so removeTab() will only
    // remove semantic/tab-bar state and cannot perform a second widget move.
    area->hide();
    if (navigationSurfaceStack
        && navigationSurfaceStack->indexOf(area) >= 0) {
        navigationSurfaceStack->removeWidget(area);
    }
    area->setProperty("threadTabbed", false);
    return tabIndex;
}

void NavigationUiController::activateTab(int index, bool restoreBookmark)
{
    const auto* entry = tabModel.at(index);
    if (!entry || !navigationTabs
        || (!backendForAccount(entry->accountId)
            && !(usesSharedView() && viewNavigationSession->allowsAccount(entry->accountId)))) {
        return;
    }

    if (activeTabIndex != index) {
        saveActiveTabLocation();

        // The previous tab now exists only as an in-memory semantic entry.
        // A hidden thread must not retain its timeline or issue new requests.
        const auto* outgoing = tabModel.at(activeTabIndex);
        ChatArea* old = outgoing && !outgoing->rootId.isEmpty()
            ? findThreadForEntry(*outgoing) : nullptr;
        if (old && old->property("threadTabbed").toBool()) {
            old->onDeactivate();
            old->hide();
            if (navigationSurfaceStack && navigationSurfaceStack->indexOf(old) >= 0)
                navigationSurfaceStack->removeWidget(old);
            old->setProperty("threadTabbed", false);
            // Tab state owns only the destination/bookmark. The QWidget is no
            // longer presented by either stack, so retire the active view
            // reference before closing it. Qt's WA_DeleteOnClose handles
            // destruction safely even when this transition originates in
            // a signal emitted by the thread's own child widget.
            if (activeArea == old) {
                activeArea = nullptr;
            }
            old->close();
        }
    }

    // The tab and Back/Forward history carry the same runtime bookmark.
    // An explicit navigation target supersedes a passive viewport snapshot.
    Location location = tabLocation(*entry);
    if (!restoreBookmark) {
        location.postId.clear();
        location.viewportAnchor.reset();
    }
    activeTabIndex = index;

    if (navigationTabs->currentIndex() != index) {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }

    const bool previousSwitching = switchingTabs;
    switchingTabs = true;

    if (!backendForAccount(location.accountId)) {
        showEmptyConversation();
        recordLocation(location, nullptr);
    } else if (!location.rootId.isEmpty()) {
        // A thread-only tab is independent, but its right pane must still not
        // show a docked thread from an unrelated channel in contextual mode.
        closeThreadDockIfDifferent(location.accountId, location.channelId);
        Backend* sourceBackend = backendForAccount(location.accountId);
        ChatArea* area = sourceBackend
            ? findThreadForBackend(*sourceBackend, location.channelId, location.rootId)
            : nullptr;
        BackendChannel* channel = sourceBackend
            ? sourceBackend->getStorage().getChannelById(location.channelId)
            : nullptr;
        if (!area && sourceBackend && channel) {
            ChatArea* parent = currentChannelPage();
            if (!parent || &parent->getChannel() != channel) {
                parent = nullptr;
            }
            area = new ChatArea(*sourceBackend, *channel, location.rootId, parent);
            if (parent) {
                parent->threadsAreas.insert(area);
            }
        }
        if (area) {
            tabifyThread(area, index);
            currentLocation = location;
            activeArea = area;
            if (!location.postId.isEmpty()) {
                restoreSessionBookmark(area, location.postId, location.viewportAnchor);
            }
        } else if (usesSharedView()) {
            showEmptyConversation();
            recordLocation(location, nullptr);
        }
    } else {
        if (navigationSurfaceStack && mainStack) {
            navigationSurfaceStack->setCurrentWidget(mainStack);
            mainStack->show();
        }
        navigateTo(location);
    }

    switchingTabs = previousSwitching;
    refreshTabBarVisibility();
    syncSplitterEdgeGutters();
}

void NavigationUiController::removeTab(int index, bool closeThread)
{
    if (!navigationTabs) {
        return;
    }
    const auto* entryPtr = tabModel.at(index);
    if (!entryPtr) {
        return;
    }

    const NavigationTabsModel::Entry removedEntry = *entryPtr;
    const bool wasActive = index == activeTabIndex;
    if (wasActive) {
        saveActiveTabLocation();
    }

    ChatArea* thread = nullptr;
    if (!removedEntry.rootId.isEmpty()) {
        thread = findThreadForEntry(removedEntry);
        if (thread && thread->property("threadTabbed").toBool()) {
            thread->hide();
            if (navigationSurfaceStack
                && navigationSurfaceStack->indexOf(thread) >= 0) {
                navigationSurfaceStack->removeWidget(thread);
            }
            thread->setProperty("threadTabbed", false);
        }
    }

    int nextIndex = -1;
    {
        QSignalBlocker blocker(navigationTabs);
        tabModel.remove(index);
        navigationTabs->removeTab(index);

        if (tabModel.isEmpty()) {
            Location fallback;
            if (ChatArea* channelArea =
                    currentChannelPage()) {
                fallback = captureLocation(channelArea);
            }
            if (!fallback.valid()) {
                fallback.accountId = removedEntry.accountId;
                fallback.channelId = removedEntry.channelId;
            }
            nextIndex = appendNavigationTab(fallback);
        } else if (wasActive) {
            nextIndex = std::min(index, tabModel.count() - 1);
        } else {
            activeTabIndex -= activeTabIndex > index ? 1 : 0;
            nextIndex = activeTabIndex;
        }

        if (nextIndex >= 0) {
            navigationTabs->setCurrentIndex(nextIndex);
        }
    }

    refreshTabBarVisibility();

    if (thread) {
        if (closeThread) {
            if (activeArea == thread) activeArea = nullptr;
            thread->close();
            thread = nullptr;
        } else {
            updateThreadButton(thread);
        }
    }

    if (wasActive && nextIndex >= 0) {
        activeTabIndex = -1;
        activateTab(nextIndex);
    }
}

void NavigationUiController::closeTab(int index)
{
    removeTab(index, true);
    scheduleSessionSave();
}

void NavigationUiController::tabifyThread(ChatArea* area, int tabIndex)
{
    if (!area || !area->isThread || !navigationSurfaceStack) {
        return;
    }

    ensureThreadButton(area);

    const bool wasDockedCurrent =
        threadStack && threadStack->currentWidget() == area;
    area->hide();
    if (threadStack && threadStack->indexOf(area) >= 0) {
        threadStack->removeWidget(area);
        if (wasDockedCurrent) {
            persistThreadPaneWidth();
            threadStack->hide();
        }
    }

    if (area->isWindow() || area->parentWidget() != navigationSurfaceStack) {
        area->setParent(navigationSurfaceStack, Qt::Widget);
    }
    if (navigationSurfaceStack->indexOf(area) < 0) {
        navigationSurfaceStack->addWidget(area);
    }

    area->setProperty("threadDetached", false);
    area->setProperty("threadTabbed", true);
    area->setSplitterEdgeGutters(false, false);
    navigationSurfaceStack->setCurrentWidget(area);
    area->show();

    activeTabIndex = tabIndex;
    updateThreadButton(area);
    recordArea(area);
    syncSplitterEdgeGutters();
}

bool NavigationUiController::activateExistingTab(
    Backend& source, const QString& channelId,
    const QString& rootId,
    bool restoreBookmark)
{
    if (backendForAccount(accountIdForBackend(source)) != &source
        || !navigationTabs || channelId.isEmpty()) {
        return false;
    }

    int index = -1;
    const auto* activeEntry = tabModel.at(activeTabIndex);
    if (activeEntry
        && activeEntry->accountId == accountIdForBackend(source)
        && activeEntry->channelId == channelId
        && activeEntry->rootId == rootId) {
        index = activeTabIndex;
    } else {
        index = tabModel.findDestination(channelId, rootId, accountIdForBackend(source));
    }

    if (index < 0) {
        return false;
    }

    {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }
    activateTab(index, restoreBookmark);
    return true;
}

void NavigationUiController::openInTab(
    Backend& source, const QString& channelId,
    const QString& rootId,
    const QString& postId,
    const QStringList& contextPostIds,
    bool reachedOldest,
    bool reachedNewest)
{
    if (backendForAccount(accountIdForBackend(source)) != &source) return;
    BackendChannel* channel = source.getStorage().getChannelById(channelId);
    if (!navigationTabs || !channel) {
        return;
    }

    ensureInitialTab();

    // The tab entry contains passive revisit state only. An explicit post target
    // must not be converted into a bookmark because bookmark restoration is
    // intentionally silent (no navigation highlight).
    Location destination;
    destination.accountId = accountIdForBackend(source);
    destination.channelId = channelId;
    destination.rootId = rootId;

    // Keep the outgoing viewport unchanged until its bookmark is captured.
    // Making the second tab visible first shrinks ChatArea, moving the centre
    // onto the adjacent post and corrupting both tab and Back history anchors.
    const int index = appendNavigationTab(destination, true);
    if (index < 0) {
        return;
    }

    {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }
    activateTab(index, postId.isEmpty());

    if (postId.isEmpty()) {
        return;
    }

    // Use the exact explicit-post path used outside tabs. It owns context
    // installation, viewport locking and highlightPostWhenReady().
    window.openChannelPostForBackend(source, channelId, postId, rootId, contextPostIds,
                           reachedOldest, reachedNewest, false);
}

void NavigationUiController::recordArea(ChatArea* area)
{
    if (!area || !canPresentBackend(area->getBackend())) {
        return;
    }
    trackSessionArea(area);
    if (!area->property("headerNavigationConnected").toBool()) {
        connect(area, &ChatArea::searchInConversationRequested, this,
                [this, area](const QString& channelId) {
            window.openMessageSearchForBackend(area->getBackend(), channelId);
        });
        area->setProperty("headerNavigationConnected", true);
    }
    if (!area->isThread) {
        ensureTabPinAction(area);
        closeThreadDockIfDifferent(accountIdForBackend(area->getBackend()),
                                       area->getChannel().id);
    }
    scheduleSessionSave();

    const Location next = captureLocation(area);
    if (!next.valid() || backendForAccount(next.accountId) != &area->getBackend()) {
        return;
    }

    if (navigationTabs && !switchingTabs) {
        const int existingIndex =
            tabModel.findDestination(next.channelId, next.rootId, next.accountId);
        if (existingIndex >= 0 && existingIndex != activeTabIndex) {
            {
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(existingIndex);
            }
            activateTab(existingIndex);
            return;
        }

        if (area->isThread && area->property("threadTabbed").toBool()) {
            const int index = tabIndexForThread(area);
            if (index >= 0) {
                updateTab(index, next);
                activeTabIndex = index;
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(index);
            }
        } else if (!area->isThread) {
            int index = -1;
            if (existingIndex == activeTabIndex && activeTabIndex >= 0) {
                index = activeTabIndex;
                updateTab(index, next);
            } else if (tabModel.isEmpty()) {
                index = appendNavigationTab(next);
            } else {
                // Ordinary channel/DM/GM navigation reuses the active channel
                // tab when it is unpinned. If the active tab is pinned (or is a
                // thread), fall back to another unpinned channel slot. Only
                // create a new tab when every existing channel tab is pinned.
                index = tabModel.findReusableChannelTab(activeTabIndex);
                if (index >= 0) {
                    updateTab(index, next);
                } else {
                    index = appendNavigationTab(next);
                }
            }

            if (index >= 0) {
                activeTabIndex = index;
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(index);
                if (navigationSurfaceStack && mainStack) {
                    navigationSurfaceStack->setCurrentWidget(mainStack);
                }
            }
            updateTabPinAction(area);
        }
        refreshTabBarVisibility();
    }

    // A contextual thread click is one semantic navigation even when its
    // parent channel has to be admitted or switched first. Preserve the
    // outgoing Back entry until the requested thread is actually presented.
    if (pendingContextualThread) {
        if (!area->isThread
            && next.accountId == pendingContextualThread->accountId
            && next.channelId == pendingContextualThread->channelId) {
            return;
        }
        if (next.sameDestination(*pendingContextualThread)) {
            pendingContextualThread.reset();
        }
    }

    // Deferred channel activation records can arrive after
    // the thread is already docked. They must not rewrite the active thread
    // location as its parent channel and add a phantom Back step.
    if (!area->isThread && !replayingHistory && currentLocation.isChat()
        && !currentLocation.rootId.isEmpty()
        && currentLocation.accountId == next.accountId
        && currentLocation.channelId == next.channelId
        && threadStack && !threadStack->isHidden()) {
        auto* docked = qobject_cast<ChatArea*>(threadStack->currentWidget());
        if (docked && &docked->getBackend() == &area->getBackend()
            && docked->root_id == currentLocation.rootId
            && docked->getChannel().id == next.channelId) {
            return;
        }
    }

    // Back/Forward can also activate a parent before restoring its dock.
    if (pendingHistoryThread && !area->isThread
        && next.accountId == pendingHistoryThread->accountId
        && next.channelId == pendingHistoryThread->channelId) {
        return;
    }
    recordLocation(next, area);
}

void NavigationUiController::recordCollection(PostCollectionView* page)
{
    const Location next = captureCollection(page);
    if (!next.valid() || next.isChat()
        || backendForAccount(next.accountId) != &page->backendInstance()) {
        return;
    }
    recordLocation(next, nullptr);
}

void NavigationUiController::recordLocation(const Location& next, ChatArea* area)
{
    if (!next.valid()) return;
    // Session restore may temporarily activate a channel before applying its
    // saved tab and history snapshot. Those QWidget events are presentation
    // activity, not new navigation; they must not rewrite the owning model.
    if (restoringSession || applyingViewSession
        || (viewNavigationSession && !sessionRestored)) {
        currentLocation = next;
        activeArea = area;
        return;
    }
    if (pendingHistoryThread
        && (!next.isChat() || next.accountId != pendingHistoryThread->accountId
            || next.channelId != pendingHistoryThread->channelId)) {
        window.beginSemanticNavigation();
        pendingHistoryThread.reset();
    }

    auto* session = historySession();
    if (!currentLocation.valid()) {
        currentLocation = next;
        activeArea = area;
        if (!replayingHistory)
            session->recordVisit(semanticLocation(next));
        updateHistoryButtons();
        return;
    }
    if (currentLocation.sameDestination(next)) {
        currentLocation = next;
        activeArea = area;
        session->updateCurrentBookmark(semanticLocation(next));
        updateHistoryButtons();
        return;
    }

    // Record the outgoing semantic anchor while its ChatArea still exists.
    // Only the session stores the Back/Forward stacks.
    if (activeArea && currentLocation.isChat()) {
        const Location latest = captureLocation(activeArea);
        if (latest.valid() && currentLocation.sameDestination(latest))
            currentLocation = latest;
    }
    if (!replayingHistory) {
        session->updateCurrentBookmark(semanticLocation(currentLocation));
        session->recordVisit(semanticLocation(next));
    } else {
        // A Back/Forward transition already updated the model history.
        session->updateCurrentBookmark(semanticLocation(next));
    }
    currentLocation = next;
    activeArea = area;
    updateHistoryButtons();
}

void NavigationUiController::updateHistoryButtons()
{
    auto* session = historySession();
    if (backButton) backButton->setEnabled(session->canGoBack());
    if (forwardButton) forwardButton->setEnabled(session->canGoForward());
    syncViewSession();
}

void NavigationUiController::goBack()
{
    auto* session = historySession();
    if (!session->canGoBack()) return;
    pendingHistoryThread.reset();
    pendingContextualThread.reset();
    window.beginSemanticNavigation();

    if (activeArea && currentLocation.isChat()) {
        const Location latest = captureLocation(activeArea);
        if (latest.valid() && currentLocation.sameDestination(latest))
            currentLocation = latest;
    }
    if (currentLocation.valid())
        session->updateCurrentBookmark(semanticLocation(currentLocation));

    const auto target = session->goBack();
    if (!target) return;
    Location destination;
    if (!presentationLocation(*target, destination)) return;
    replayingHistory = true;
    navigateTo(destination);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::goForward()
{
    auto* session = historySession();
    if (!session->canGoForward()) return;
    pendingHistoryThread.reset();
    pendingContextualThread.reset();
    window.beginSemanticNavigation();

    if (activeArea && currentLocation.isChat()) {
        const Location latest = captureLocation(activeArea);
        if (latest.valid() && currentLocation.sameDestination(latest))
            currentLocation = latest;
    }
    if (currentLocation.valid())
        session->updateCurrentBookmark(semanticLocation(currentLocation));

    const auto target = session->goForward();
    if (!target) return;
    Location destination;
    if (!presentationLocation(*target, destination)) return;
    replayingHistory = true;
    navigateTo(destination);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::showEmptyConversation()
{
    if (!mainStack) return;
    if (auto* previous = currentChannelPage())
        previous->onDeactivate();
    closeThreadDockIfDifferent({}, {}, true);
    if (!emptyConversation) {
        emptyConversation = new QWidget(mainStack);
        emptyConversation->setObjectName(QStringLiteral("emptyConversationPage"));
        mainStack->addWidget(emptyConversation);
    }
    if (navigationSurfaceStack)
        navigationSurfaceStack->setCurrentWidget(mainStack);
    mainStack->setCurrentWidget(emptyConversation);
    activeArea = nullptr;
}

void NavigationUiController::navigateTo(const Location& location)
{
    if (!location.valid()) return;
    if (!backendForAccount(location.accountId)) {
        if (usesSharedView() && viewNavigationSession->allowsAccount(location.accountId)) {
            showEmptyConversation();
            recordLocation(location, nullptr);
        }
        return;
    }

    if (!location.isChat()) {
        PostCollectionView* page = findCollection(location);
        if (!page || !mainStack) {
            return;
        }
        if (navigationSurfaceStack) {
            navigationSurfaceStack->setCurrentWidget(mainStack);
        }
        mainStack->setCurrentWidget(page);
        mainStack->show();
        currentLocation = location;
        activeArea = nullptr;
        return;
    }

    Backend* sourceBackend = backendForAccount(location.accountId);
    if (!sourceBackend || !channelTree) {
        return;
    }

    BackendChannel* channel = sourceBackend->getStorage().getChannelById(location.channelId);
    if (!channel) {
        if (usesSharedView()) {
            showEmptyConversation();
            recordLocation(location, nullptr);
        }
        return;
    }

    ChatArea* area = nullptr;
    if (location.rootId.isEmpty()) {
        channelTree->openStoredChannelForAccount(
            location.channelId, channelTree->accountIdForBackend(sourceBackend));
        area = currentChannelPage();
        if (!area || &area->getChannel() != channel) {
            return;
        }
        // currentChanged() is only emitted when the physical stack page
        // changes. Back to an already selected channel must still retire an
        // unrelated dock, even if its widget never changed.
        // In contextual mode a history entry for a channel-only location
        // must close its dock, even if it belongs to that same channel.
        // Independent-pane mode deliberately retains its old presentation.
        const bool restoreChannelOnly = replayingHistory
            && MLOptions::instance()->value<bool>(
                THREAD_SIDEBAR_FOLLOWS_CHANNEL,
                THREAD_SIDEBAR_FOLLOWS_CHANNEL_DEFAULT);
        closeThreadDockIfDifferent(location.accountId, location.channelId, restoreChannelOnly);
        recordArea(area);
    } else {
        // A Back/Forward target already represented by a thread-only tab must
        // activate that tab, not reconstruct the thread as a foreign dock.
        const int existingThreadTab = tabModel.findDestination(
            location.channelId, location.rootId, location.accountId);
        if (existingThreadTab >= 0) {
            activateTab(existingThreadTab, location.postId.isEmpty());
            currentLocation = location;
            activeArea = findThreadForBackend(*sourceBackend, location.channelId, location.rootId);
            if (activeArea && !location.postId.isEmpty()) {
                restoreSessionBookmark(activeArea, location.postId,
                                       location.viewportAnchor);
            }
            return;
        }

        area = findThreadForBackend(*sourceBackend, location.channelId, location.rootId);
        const bool independentThread =
            area && (area->property("threadTabbed").toBool()
                     || area->property("threadDetached").toBool());
        const bool contextualMode = MLOptions::instance()->value<bool>(
            THREAD_SIDEBAR_FOLLOWS_CHANNEL,
            THREAD_SIDEBAR_FOLLOWS_CHANNEL_DEFAULT);
        const bool needsParentTab = chooseThreadParentTab(
            contextualMode, tabModel, activeTabIndex,
            location.channelId, location.rootId, location.accountId).route != ThreadParentTabRoute::KeepCurrent;
        if (!independentThread
            && (needsParentTab
                || shouldOpenParentChannelForThread(*sourceBackend, location.channelId))) {
            // History must obey the same cross-channel tab invariant as
            // regular navigation, even when starting from a thread-only tab.
            // MainWindow handles asynchronous channel admission and restores
            // the pending bookmark after the parent channel settles.
            pendingHistoryThread = location;
            currentLocation = location;
            activeArea = nullptr;
            window.openChannelPostForBackend(*sourceBackend, location.channelId, QString(), location.rootId,
                                   {}, false, false, true);
            return;
        }
        if (!area) {
            ChatArea* parent = currentChannelPage();
            if (!parent || &parent->getChannel() != channel) {
                parent = nullptr;
            }
            area = new ChatArea(*sourceBackend, *channel, location.rootId, parent);
            if (parent) {
                parent->threadsAreas.insert(area);
            }
        }
        presentThread(area);
    }

    // activateTab() may display the parent channel as a preparation step for
    // an ordinary thread click. Do not replace its actual history origin.
    if (!(pendingContextualThread && location.rootId.isEmpty()
          && location.accountId == pendingContextualThread->accountId
          && location.channelId == pendingContextualThread->channelId)) {
        currentLocation = location;
        activeArea = area;
    }
    if (location.postId.isEmpty() || !area) {
        return;
    }

    // Passive bookmarks resolve cold bodies without flashing a permalink or
    // changing the current semantic destination.
    restoreSessionBookmark(area, location.postId, location.viewportAnchor);
}

bool NavigationUiController::isConversationVisible(
    const QString& channelId, const QString& rootId) const
{
    const Backend* owner = backend();
    return owner && isConversationVisible(*owner, channelId, rootId);
}

bool NavigationUiController::isConversationVisible(
    const Backend& source, const QString& channelId, const QString& rootId) const
{
    ChatArea* area = nullptr;
    if (rootId.isEmpty()) {
        // A docked thread may be active while its parent channel remains
        // visible in the adjacent central surface.
        area = mainStack
            ? qobject_cast<ChatArea*>(mainStack->currentWidget()) : nullptr;
        if (!area || area->isThread) return false;
    } else {
        area = findThreadForBackend(source, channelId, rootId);
    }

    if (!area || &area->getBackend() != &source || area->getChannel().id != channelId
        || !area->isVisible() || !area->window()) {
        return false;
    }
    // QWidget::isActiveWindow() alone is not sufficient: all widgets inside
    // an inactive QStackedWidget may still belong to the active top-level
    // window. isVisible() is the second, essential half of this check.
    return area->window()->isActiveWindow();
}

ChatArea* NavigationUiController::findThread(const QString& channelId,
                                             const QString& rootId) const
{
    const Backend* owner = backend();
    return owner ? findThreadForBackend(*owner, channelId, rootId) : nullptr;
}

ChatArea* NavigationUiController::findThreadForEntry(const NavigationTabsModel::Entry& entry) const
{
    const Backend* owner = backendForAccount(entry.accountId);
    return owner ? findThreadForBackend(*owner, entry.channelId, entry.rootId) : nullptr;
}

ChatArea* NavigationUiController::findThreadForBackend(
    const Backend& owner, const QString& channelId, const QString& rootId) const
{
    if (channelId.isEmpty() || rootId.isEmpty()) {
        return nullptr;
    }

    const auto matches = [&owner, &channelId, &rootId](ChatArea* area) {
        return area && &area->getBackend() == &owner
            && area->isThread && area->root_id == rootId
            && area->getChannel().id == channelId;
    };

    // The two stacks own embedded thread presentations. A removed widget
    // is not an open thread, regardless of whether QObject destruction has
    // completed in the current event loop.
    for (QStackedWidget* stack : {navigationSurfaceStack, threadStack}) {
        if (!stack) continue;
        for (int i = 0; i < stack->count(); ++i) {
            auto* area = qobject_cast<ChatArea*>(stack->widget(i));
            if (matches(area)) return area;
        }
    }

    // Detached threads have independent QWidget lifetimes and are the only
    // valid thread presentations outside those stacks.
    for (QWidget* topLevelWidget : QApplication::topLevelWidgets()) {
        auto* area = qobject_cast<ChatArea*>(topLevelWidget);
        if (matches(area) && area->property("threadDetached").toBool()
            && area->isVisible()) {
            return area;
        }
    }
    return nullptr;
}

void NavigationUiController::ensureTabPinAction(ChatArea* area)
{
    if (!area || area->isThread) {
        return;
    }

    QMenu* menu = area->headerActionsMenu();
    if (!menu) {
        return;
    }

    auto* action = area->findChild<QAction*>(QStringLiteral("tabPinAction"));
    if (!action) {
        action = new QAction(area);
        action->setObjectName(QStringLiteral("tabPinAction"));
        // A checkable QAction makes QMenu reserve a checkmark column,
        // shifting every other menu item. Use the existing pin-tab icon
        // and a Pin/Unpin label instead.
        action->setIcon(IconUtils::symbolicIcon(
            QStringLiteral(":/icons/tab-pin")));
        action->setProperty("matterleastMenuIconPath",
                            QStringLiteral(":/icons/tab-pin"));

        QAction* before = nullptr;
        for (QAction* candidate : menu->actions()) {
            if (candidate
                && candidate->objectName() == QStringLiteral("searchConversationAction")) {
                before = candidate;
                break;
            }
        }
        if (before) {
            menu->insertAction(before, action);
        } else {
            menu->addAction(action);
        }

        connect(action, &QAction::triggered, this,
                [this, area] {
            const int index = tabModel.findDestination(
                area->getChannel().id, QString(), accountIdForBackend(area->getBackend()));
            if (const auto* entry = tabModel.at(index)) {
                tabModel.setPinned(index, !entry->pinned);
                scheduleSessionSave();
            }
            updateTabPinAction(area);
        });
    }

    updateTabPinAction(area);
}

void NavigationUiController::updateTabPinAction(ChatArea* area)
{
    if (!area || area->isThread) {
        return;
    }

    auto* action = area->findChild<QAction*>(QStringLiteral("tabPinAction"));
    if (!action) {
        return;
    }

    const int index = tabModel.findDestination(area->getChannel().id, QString(),
                                               accountIdForBackend(area->getBackend()));
    const auto* entry = tabModel.at(index);
    const bool available = entry && entry->rootId.isEmpty();
    const bool pinned = available && entry->pinned;

    action->setVisible(available);
    action->setEnabled(available);
    action->setText(pinned ? tr("Unpin tab") : tr("Pin tab"));
}

void NavigationUiController::ensureThreadButton(ChatArea* area)
{
    if (!area || !area->isThread) {
        return;
    }
    trackSessionArea(area);

    auto* layout = area->findChild<QHBoxLayout*>(QStringLiteral("propertieslLayout"));
    if (!layout) {
        return;
    }

    auto addHeaderButton = [area, layout](QToolButton* button) {
        auto* actionsButton = area->findChild<QToolButton*>(
            QStringLiteral("chatActionsButton"));
        const int actionsIndex = actionsButton ? layout->indexOf(actionsButton) : -1;
        if (actionsIndex >= 0) {
            layout->insertWidget(actionsIndex, button, 0, Qt::AlignVCenter);
        } else {
            layout->addWidget(button, 0, Qt::AlignVCenter);
        }
    };

    auto* tabButton = area->findChild<QToolButton*>(
        QStringLiteral("threadTabButton"));
    if (!tabButton) {
        tabButton = new QToolButton(area);
        tabButton->setObjectName(QStringLiteral("threadTabButton"));
        tabButton->setAutoRaise(true);
        tabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        tabButton->setIconSize(QSize(16, 16));
        tabButton->setCursor(Qt::PointingHandCursor);
        addHeaderButton(tabButton);

        connect(tabButton, &QToolButton::clicked, this, [this, area] {
            QPointer<ChatArea> guard(area);
            QTimer::singleShot(0, this, [this, guard] {
                if (!guard) {
                    return;
                }
                if (guard->property("threadTabbed").toBool()) {
                    attachThread(guard);
                    return;
                }
                AppNavigationService::instance(guard->getBackend()).openThreadInTab(
                    guard->getChannel().id, guard->root_id);
            });
        });
    }

    auto* presentationButton = area->findChild<QToolButton*>(
        QStringLiteral("threadPresentationButton"));
    if (!presentationButton) {
        presentationButton = new QToolButton(area);
        presentationButton->setObjectName(QStringLiteral("threadPresentationButton"));
        presentationButton->setAutoRaise(true);
        presentationButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        presentationButton->setIconSize(QSize(16, 16));
        presentationButton->setCursor(Qt::PointingHandCursor);
        addHeaderButton(presentationButton);

        connect(presentationButton, &QToolButton::clicked, this, [this, area] {
            QPointer<ChatArea> guard(area);
            QTimer::singleShot(0, this, [this, guard] {
                if (!guard) {
                    return;
                }
                if (guard->property("threadDetached").toBool()) {
                    attachThread(guard);
                } else {
                    detachThread(guard);
                }
            });
        });
    }

    auto* closeButton = area->findChild<QToolButton*>(QStringLiteral("threadCloseButton"));
    if (!closeButton) {
        closeButton = new QToolButton(area);
        closeButton->setObjectName(QStringLiteral("threadCloseButton"));
        closeButton->setAutoRaise(true);
        closeButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        closeButton->setIconSize(QSize(16, 16));
        closeButton->setCursor(Qt::PointingHandCursor);
        closeButton->setIcon(area->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        const QString label = tr("Close thread");
        closeButton->setToolTip(label);
        closeButton->setAccessibleName(label);
        addHeaderButton(closeButton);

        connect(closeButton, &QToolButton::clicked, this, [this, area] {
            if (!area) {
                return;
            }

            if (area->property("threadTabbed").toBool()) {
                const int index = tabIndexForThread(area);
                if (index >= 0) {
                    closeTab(index);
                    return;
                }
            }

            const bool wasActive = activeArea == area;
            bool wasDockedCurrent = false;
            if (threadStack && threadStack->indexOf(area) >= 0) {
                wasDockedCurrent = threadStack->currentWidget() == area;
                area->hide();
                threadStack->removeWidget(area);
                if (wasDockedCurrent) {
                    persistThreadPaneWidth();
                    threadStack->hide();
                }
            }

            if (wasDockedCurrent) {
                syncSplitterEdgeGutters();
            }
            area->setSplitterEdgeGutters(false, false);
            if (wasActive) activeArea = nullptr;
            area->close();
            if (wasActive) {
                QTimer::singleShot(0, this, [this] {
                    if (channelTree) {
                        recordArea(currentChannelPage());
                    }
                });
            }
        });
    }

    updateThreadButton(area);
}

void NavigationUiController::updateThreadButton(ChatArea* area)
{
    if (!area) {
        return;
    }
    auto* button = area->findChild<QToolButton*>(
        QStringLiteral("threadPresentationButton"));
    if (!button) {
        return;
    }

    const bool detached = area->property("threadDetached").toBool();
    const QString label = detached ? tr("Attach thread") : tr("Detach thread");
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setIcon(threadPresentationIcon(button->palette()));

    if (auto* tabButton = area->findChild<QToolButton*>(
            QStringLiteral("threadTabButton"))) {
        const bool tabbed = area->property("threadTabbed").toBool();
        const QString tabLabel = tabbed ? tr("Attach thread")
                                       : tr("Open thread in new tab");
        tabButton->setToolTip(tabLabel);
        tabButton->setAccessibleName(tabLabel);
        tabButton->setIcon(threadTabIcon(tabButton->palette()));
    }
}

void NavigationUiController::attachThread(ChatArea* area)
{
    if (!area || !threadStack || !contentSplitter) {
        return;
    }

    ensureThreadButton(area);

    const int releasedTabIndex = releaseThreadFromTabSurface(area);
    if (releasedTabIndex >= 0) {
        // Removing the tab already selects the next central destination. Do not
        // force a channel tab here: a thread tab in the central surface is fully
        // compatible with another thread docked on the right.
        removeTab(releasedTabIndex, false);
    }

    if (auto* previous = qobject_cast<ChatArea*>(threadStack->currentWidget());
        previous && previous != area) {
        // Stacked widgets historically kept every previously opened dock
        // alive, causing hundreds of hidden timelines to be saved/restored.
        // Only the currently visible dock is a presentation; persist the
        // previous draft, then release its message list and residency leases.
        previous->onDeactivate();
        previous->setSplitterEdgeGutters(false, false);
        previous->hide();
        threadStack->removeWidget(previous);
        if (activeArea == previous) activeArea = nullptr;
        previous->close(); // WA_DeleteOnClose; safe while signals unwind
    }

    area->hide();
    if (threadStack->indexOf(area) < 0) {
        area->setParent(threadStack, Qt::Widget);
        threadStack->addWidget(area);
    }
    area->setProperty("threadDetached", false);
    area->setProperty("threadTabbed", false);
    threadStack->setCurrentWidget(area);

    // A hidden splitter child is reported as size 0 regardless of the width
    // encoded in restoreState(). Make the pane visible first, then decide
    // whether the restored/current geometry is actually usable.
    threadStack->show();
    area->show();
    syncSplitterEdgeGutters();

    bool restored = restoreThreadPaneWidth(*contentSplitter,
                                           _preferredThreadPaneWidth);
    if (!restored && !threadSplitterStateRestored
        && !_legacyThreadSplitterState.isEmpty()) {
        restored = contentSplitter->restoreState(_legacyThreadSplitterState);
    }
    ensureThreadPaneExpanded(*contentSplitter,
                             !restored && !threadSplitterStateRestored);
    threadSplitterStateRestored = true;
    updateThreadButton(area);
    recordArea(area);
}

void NavigationUiController::detachThread(ChatArea* area)
{
    if (!area || !threadStack) {
        return;
    }

    const int releasedTabIndex = releaseThreadFromTabSurface(area);

    const bool wasCurrent = threadStack->currentWidget() == area;
    area->hide();
    if (threadStack->indexOf(area) >= 0) {
        threadStack->removeWidget(area);
    }

    area->setSplitterEdgeGutters(false, false);

    // Complete the QWidget state transition before removing the semantic tab.
    // removeTab() can then activate the replacement central tab without racing
    // another reparent of this thread.
    area->setParent(nullptr, Qt::Window);
    area->setAttribute(Qt::WA_DeleteOnClose, true);
    area->setProperty("threadDetached", true);
    area->setProperty("threadTabbed", false);
    if (viewNavigationSession)
        area->setProperty("navigationViewId", viewNavigationSession->viewId());
    updateThreadButton(area);

    if (releasedTabIndex >= 0) {
        removeTab(releasedTabIndex, false);
    }

    if (wasCurrent) {
        persistThreadPaneWidth();
        threadStack->hide();
    }
    syncSplitterEdgeGutters();
    area->show();
    area->raise();
    area->activateWindow();
    recordArea(area);
}

void NavigationUiController::rememberThreadPaneWidth()
{
    if (!contentSplitter) {
        return;
    }
    const int width = expandedThreadPaneWidth(*contentSplitter);
    if (width > 0) {
        _preferredThreadPaneWidth = width;
    }
}

void NavigationUiController::persistThreadPaneWidth()
{
    rememberThreadPaneWidth();
    if (_preferredThreadPaneWidth > 0) {
        MLOptions::instance()->setValue(QStringLiteral("content_thread_width"),
                                        _preferredThreadPaneWidth);
    }
    // Keep the legacy splitter state only when both panes have real widths.
    // This avoids replacing a good layout with a collapsed, hidden thread.
    if (contentSplitter && expandedThreadPaneWidth(*contentSplitter) > 0) {
        MLOptions::instance()->setValue(
            QStringLiteral("content_splitter_state"),
            contentSplitter->saveState());
    }
}

void NavigationUiController::syncSplitterEdgeGutters()
{
    ChatArea* centralArea = nullptr;
    if (navigationSurfaceStack) {
        QWidget* centralSurface = navigationSurfaceStack->currentWidget();
        if (centralSurface == mainStack) {
            centralArea = mainStack
                ? qobject_cast<ChatArea*>(mainStack->currentWidget())
                : nullptr;
        } else {
            centralArea = qobject_cast<ChatArea*>(centralSurface);
        }
    } else if (mainStack) {
        centralArea = qobject_cast<ChatArea*>(mainStack->currentWidget());
    }

    auto* threadArea = threadStack
        ? qobject_cast<ChatArea*>(threadStack->currentWidget())
        : nullptr;
    const bool threadDocked = threadStack && !threadStack->isHidden()
        && threadArea && !threadArea->property("threadDetached").toBool();

    if (centralArea) {
        // Whatever occupies the central surface touches the outer/sidebar edge
        // on the left and the thread splitter only while a docked pane is open.
        centralArea->setSplitterEdgeGutters(true, threadDocked);
    }
    if (threadArea && threadDocked) {
        threadArea->setSplitterEdgeGutters(true, false);
    }
}

bool NavigationUiController::hasIndependentThreadPresentation(
    Backend& source, const QString& channelId, const QString& rootId) const
{
    if (backendForAccount(accountIdForBackend(source)) != &source
        || channelId.isEmpty() || rootId.isEmpty()) {
        return false;
    }
    const auto* active = tabModel.at(activeTabIndex);
    if (active && active->accountId == accountIdForBackend(source)
        && active->channelId == channelId && active->rootId == rootId) {
        return true;
    }
    const ChatArea* thread = findThreadForBackend(source, channelId, rootId);
    return thread && thread->property("threadDetached").toBool();
}

void NavigationUiController::selectContextualThreadTab(
    Backend& source, const QString& channelId, const QString& rootId)
{
    if (backendForAccount(accountIdForBackend(source)) != &source
        || !navigationTabs || hasIndependentThreadPresentation(source, channelId, rootId)
        || !MLOptions::instance()->value<bool>(
            THREAD_SIDEBAR_FOLLOWS_CHANNEL,
            THREAD_SIDEBAR_FOLLOWS_CHANNEL_DEFAULT)) {
        return;
    }

    ensureInitialTab();
    Location target;
    target.accountId = accountIdForBackend(source);
    target.channelId = channelId;
    target.rootId = rootId;
    const auto decision = chooseThreadParentTab(
        true, tabModel, activeTabIndex, target.channelId, target.rootId, target.accountId);
    if (decision.route != ThreadParentTabRoute::KeepCurrent
        && !pendingHistoryThread
        && (!pendingContextualThread || !pendingContextualThread->sameDestination(target))) {
        pendingContextualThread = target;
    }
    if (decision.route == ThreadParentTabRoute::KeepCurrent) {
        return;
    }
    if (decision.route == ThreadParentTabRoute::ActivateExisting) {
        activateTab(decision.index);
        return;
    }

    if (decision.route == ThreadParentTabRoute::ReuseUnpinned) {
        const auto* entry = tabModel.at(decision.index);
        if (!entry || entry->pinned) {
            return;
        }

        if (entry->rootId.isEmpty()) {
            // ChannelTree's existing recordArea() path already replaces the
            // current unpinned channel tab. Activate a different reusable
            // channel tab first, if needed, then let MainWindow open the
            // requested parent channel normally.
            if (decision.index != activeTabIndex) {
                activateTab(decision.index);
            }
            return;
        }

        // An unpinned thread-only tab is reusable too. Its central ChatArea
        // must be retired before changing the semantic tab entry; otherwise
        // activateTab() would see the *new* channel identity and leave the
        // old thread view alive in navigationSurfaceStack.
        if (decision.index == activeTabIndex) {
            saveActiveTabLocation();
        }
        if (ChatArea* old = findThreadForEntry(*entry);
            old && old->property("threadTabbed").toBool()) {
            old->onDeactivate();
            old->hide();
            if (navigationSurfaceStack
                && navigationSurfaceStack->indexOf(old) >= 0) {
                navigationSurfaceStack->removeWidget(old);
            }
            old->setProperty("threadTabbed", false);
            if (activeArea == old) {
                activeArea = nullptr;
            }
            old->close();
        }

        Location parent;
        parent.accountId = target.accountId;
        parent.channelId = channelId;
        updateTab(decision.index, parent);
        if (decision.index == activeTabIndex) {
            // Force a fresh channel presentation after replacing the current
            // thread-only entry, without snapshotting its retired view again.
            activeTabIndex = -1;
        }
        activateTab(decision.index);
        scheduleSessionSave();
        return;
    }

    // All existing tabs are pinned: preserve them and create a separate tab
    // for the thread's parent channel. Explicit thread-only tabs bypass this.
    Location parent;
    parent.accountId = target.accountId;
    parent.channelId = channelId;
    const int index = appendNavigationTab(parent, true);
    if (index >= 0) {
        activateTab(index);
    }
}

bool NavigationUiController::shouldOpenParentChannelForThread(
    Backend& source, const QString& channelId) const
{
    if (!MLOptions::instance()->value<bool>(
            THREAD_SIDEBAR_FOLLOWS_CHANNEL,
            THREAD_SIDEBAR_FOLLOWS_CHANNEL_DEFAULT)) {
        return false;
    }

    const bool centralSurfaceIsMainStack = mainStack && navigationSurfaceStack
        && navigationSurfaceStack->currentWidget() == mainStack;
    auto* centralArea = centralSurfaceIsMainStack
        ? qobject_cast<ChatArea*>(mainStack->currentWidget()) : nullptr;
    return shouldOpenThreadParentChannel(true, centralSurfaceIsMainStack,
                                         centralArea ? centralArea->getChannel().id : QString(),
                                         channelId,
                                         centralArea ? accountIdForBackend(centralArea->getBackend()) : QString(),
                                         accountIdForBackend(source));
}

void NavigationUiController::closeThreadDockIfDifferent(const QString& accountId,
                                                         const QString& channelId,
                                                         bool force)
{
    if (!threadStack || threadStack->isHidden()) return;
    auto* docked = qobject_cast<ChatArea*>(threadStack->currentWidget());
    if (!docked || (!force && !shouldCloseThreadDockForChannel(
            MLOptions::instance()->value<bool>(
                THREAD_SIDEBAR_FOLLOWS_CHANNEL,
                THREAD_SIDEBAR_FOLLOWS_CHANNEL_DEFAULT),
            channelId, docked->getChannel().id, accountId,
            accountIdForBackend(docked->getBackend())))) {
        return;
    }

    // Closing, rather than merely hiding, releases the old timeline and avoids
    // restoring an unrelated dock from the session snapshot. Drafts are saved
    // by the regular ChatArea deactivation path.
    persistThreadPaneWidth();
    docked->onDeactivate();
    docked->setSplitterEdgeGutters(false, false);
    docked->hide();
    threadStack->removeWidget(docked);
    threadStack->hide();
    if (activeArea == docked) activeArea = nullptr;
    docked->close();
    syncSplitterEdgeGutters();
    scheduleSessionSave();
}

void NavigationUiController::presentChannel(ChatArea* area)
{
    if (!area || area->isThread) {
        return;
    }

    if (navigationSurfaceStack && mainStack) {
        navigationSurfaceStack->setCurrentWidget(mainStack);
        mainStack->show();
    }

    recordArea(area);
    refreshTabBarVisibility();
    syncSplitterEdgeGutters();
}

void NavigationUiController::presentThread(ChatArea* area)
{
    if (!area || !area->isThread) {
        return;
    }

    ensureThreadButton(area);
    if (area->property("threadTabbed").toBool()) {
        const int index = tabIndexForThread(area);
        if (index >= 0) {
            {
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(index);
            }
            activateTab(index);
        }
        return;
    }
    if (area->property("threadDetached").toBool()) {
        area->show();
        area->raise();
        area->activateWindow();
        recordArea(area);
        return;
    }

    const bool restoringHistory = pendingHistoryThread
        && pendingHistoryThread->accountId == accountIdForBackend(area->getBackend())
        && pendingHistoryThread->channelId == area->getChannel().id
        && pendingHistoryThread->rootId == area->root_id;
    if (!restoringHistory) {
        pendingHistoryThread.reset();
        attachThread(area);
        return;
    }

    const Location target = *pendingHistoryThread;
    const bool wasReplaying = replayingHistory;
    replayingHistory = true;
    attachThread(area);
    replayingHistory = wasReplaying;
    currentLocation = target;
    activeArea = area;

    // MainWindow may call goToNewest() immediately after presenting a freshly
    // materialized thread. Apply the passive history bookmark afterwards so
    // its exact viewport anchor wins, without a permalink highlight.
    QPointer<ChatArea> guard(area);
    QTimer::singleShot(0, this, [this, guard, target] {
        if (!pendingHistoryThread
            || !pendingHistoryThread->sameDestination(target)) {
            return;
        }
        pendingHistoryThread.reset();
        if (guard && currentLocation.sameDestination(target)
            && !target.postId.isEmpty()) {
            restoreSessionBookmark(guard, target.postId, target.viewportAnchor);
        }
    });
}

bool NavigationUiController::eventFilter(QObject* watched, QEvent* event)
{
    if (shuttingDown)
        return QObject::eventFilter(watched, event);
    if (event && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        auto* area = qobject_cast<ChatArea*>(watched);
        if (area && area->property("sessionTracked").toBool()
            && area->property("threadDetached").toBool()) scheduleSessionSave();
    }
    if (watched == navigationTabs && event
        && event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            const int index = navigationTabs->tabAt(mouse->pos());
            if (index >= 0) {
                // Thread tabs may need reparenting as they close. Let Qt finish
                // dispatching the tab-bar mouse gesture before changing parents.
                QTimer::singleShot(0, this, [this, index] { closeTab(index); });
                event->accept();
                return true;
            }
        }
    }

    // Only a mouse release needs the sidebar viewport. Other events include
    // child destruction while the main window is tearing down that sidebar.
    if (event && event->type() == QEvent::MouseButtonRelease && channelTree
        && (watched == channelTree || watched == channelTree->viewport())) {
        QTimer::singleShot(0, this, [this] {
            if (channelTree) {
                recordArea(currentChannelPage());
            }
        });
    }

    if (event && event->type() == QEvent::Show) {
        auto* area = qobject_cast<ChatArea*>(watched);
        if (area && area->isThread
            && !area->property("threadDetached").toBool()
            && !area->property("threadTabbed").toBool()
            && (!threadStack || threadStack->indexOf(area) < 0)) {
            QPointer<ChatArea> guard(area);
            QTimer::singleShot(0, this, [this, guard] {
                if (guard
                    && !guard->property("threadDetached").toBool()
                    && !guard->property("threadTabbed").toBool()) {
                    attachThread(guard);
                }
            });
        }
    }

    return QObject::eventFilter(watched, event);
}

} // namespace Mattermost

namespace {

class NavigationUiBootstrap final : public QObject
{
public:
    explicit NavigationUiBootstrap(QObject* parent)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event && event->type() == QEvent::Show) {
            if (auto* window = qobject_cast<Mattermost::MainWindow*>(watched)) {
                if (!window->property("navigationUiSetupScheduled").toBool()) {
                    window->setProperty("navigationUiSetupScheduled", true);
                    QPointer<Mattermost::MainWindow> guard(window);
                    QTimer::singleShot(0, window, [guard] {
                        if (guard) {
                            Mattermost::NavigationUiController::instance(*guard);
                        }
                    });
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void installNavigationUiBootstrap()
{
    if (!qApp) {
        return;
    }
    auto* bootstrap = new NavigationUiBootstrap(qApp);
    qApp->installEventFilter(bootstrap);
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(installNavigationUiBootstrap)