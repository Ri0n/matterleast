#include "NavigationUiController.h"

#include <algorithm>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "mainwindow.h"
#include "options/MLOptions.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ThreadPaneLayout.h"
#include "ui/ThinSplitter.h"

namespace Mattermost {
namespace {

constexpr int MaxNavigationHistory = 100;

void trimHistory(QVector<NavigationUiController::Location>& history)
{
    if (history.size() > MaxNavigationHistory) {
        history.remove(0, history.size() - MaxNavigationHistory);
    }
}

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
    setupMainWindow();
    qApp->installEventFilter(this);
}

NavigationUiController::~NavigationUiController()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    if (contentSplitter) {
        MLOptions::instance()->setValue(
            QStringLiteral("content_splitter_state"), contentSplitter->saveState());
    }
}

void NavigationUiController::setupMainWindow()
{
    channelTree = window.findChild<ChannelTree*>(QStringLiteral("channelList"));
    mainStack = window.findChild<QStackedWidget*>(QStringLiteral("chatAreaStackedWidget"));

    setupSidebarHeader();
    setupThreadPane();

    if (mainStack) {
        connect(mainStack, &QStackedWidget::currentChanged, this,
                [this](int index) {
            syncSplitterEdgeGutters();

            QWidget* page = mainStack->widget(index);
            const bool chatSurface = qobject_cast<ChatArea*>(page) != nullptr;
            if (!chatSurface) {
                // Saved/Drafts/Search still use MainWindow's existing transient
                // collection surface. A tabbed thread must not cover a newly
                // requested collection merely because that thread currently
                // owns the navigation surface stack.
                if (!switchingTabs && navigationSurfaceStack && contentSplitter) {
                    navigationSurfaceStack->setCurrentWidget(contentSplitter);
                }
                if (navigationTabs) {
                    navigationTabs->hide();
                }
                return;
            }

            refreshTabBarVisibility();
        });
    }

    if (channelTree) {
        connect(channelTree, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem*, QTreeWidgetItem*) {
            QTimer::singleShot(0, this, [this] {
                if (channelTree) {
                    recordArea(channelTree->getCurrentPage());
                }
            });
        });
        channelTree->installEventFilter(this);
        if (channelTree->viewport()) {
            channelTree->viewport()->installEventFilter(this);
        }
        recordArea(channelTree->getCurrentPage());
    }

    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        if (contentSplitter) {
            MLOptions::instance()->setValue(
            QStringLiteral("content_splitter_state"), contentSplitter->saveState());
        }
    });

    updateIdentityTooltip();
    updateHistoryButtons();
}

void NavigationUiController::setupSidebarHeader()
{
    auto* header = window.findChild<QWidget*>(QStringLiteral("lefttop_frame"));
    auto* searchButton = window.findChild<QToolButton*>(QStringLiteral("searchButton"));
    auto* username = window.findChild<QLabel*>(QStringLiteral("usernameLabel"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("statusLabel"));
    if (username) {
        username->hide();
    }
    if (status) {
        // PresenceStatusLabel keeps forwarding status changes into the avatar;
        // hiding the textual compatibility widget does not disable that path.
        status->hide();
    }
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

    contentHost = new QWidget(sidebarSplitter);
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
    navigationTabs->hide();
    hostLayout->addWidget(navigationTabs);

    navigationSurfaceStack = new QStackedWidget(contentHost);
    navigationSurfaceStack->setObjectName(QStringLiteral("navigationSurfaceStack"));
    hostLayout->addWidget(navigationSurfaceStack, 1);

    contentSplitter = new ThinSplitter(Qt::Horizontal, navigationSurfaceStack);
    contentSplitter->setObjectName(QStringLiteral("contentSplitter"));
    contentSplitter->setChildrenCollapsible(true);
    contentSplitter->setOpaqueResize(true);

    contentSplitter->addWidget(mainStack);
    threadStack = new QStackedWidget(contentSplitter);
    threadStack->setObjectName(QStringLiteral("threadStack"));
    threadStack->setMinimumWidth(280);
    threadStack->hide();
    contentSplitter->addWidget(threadStack);
    contentSplitter->setStretchFactor(0, 2);
    contentSplitter->setStretchFactor(1, 1);

    navigationSurfaceStack->addWidget(contentSplitter);
    navigationSurfaceStack->setCurrentWidget(contentSplitter);

    sidebarSplitter->insertWidget(std::max(0, oldIndex), contentHost);
    if (!outerSizes.isEmpty()) {
        sidebarSplitter->setSizes(outerSizes);
    }

    const QByteArray state = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("content_splitter_state"));
    if (!state.isEmpty()) {
        threadSplitterStateRestored = contentSplitter->restoreState(state);
    }
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
    });
}

void NavigationUiController::updateIdentityTooltip()
{
    auto* avatar = window.findChild<QWidget*>(QStringLiteral("usericon_label"));
    if (!avatar) {
        return;
    }

    Backend* sourceBackend = backend();
    if (!sourceBackend) {
        if (auto* username = window.findChild<QLabel*>(QStringLiteral("usernameLabel"))) {
            if (!username->text().isEmpty()) {
                avatar->setToolTip(username->text());
            }
        }
        QTimer::singleShot(500, this, &NavigationUiController::updateIdentityTooltip);
        return;
    }

    const BackendUser& user = sourceBackend->getLoginUser();
    const QString displayName = user.getDisplayName().trimmed();
    const QString accountName = user.username.trimmed();
    QStringList lines;
    if (!displayName.isEmpty()) {
        lines.push_back(displayName);
    }
    if (!accountName.isEmpty()
        && QString::compare(displayName, accountName, Qt::CaseInsensitive) != 0) {
        lines.push_back(QStringLiteral("@") + accountName);
    }
    if (lines.isEmpty() && !accountName.isEmpty()) {
        lines.push_back(accountName);
    }
    avatar->setToolTip(lines.join(QLatin1Char('\n')));
}

Backend* NavigationUiController::backend() const
{
    return channelTree ? channelTree->backendInstance() : nullptr;
}

NavigationUiController::Location
NavigationUiController::captureLocation(ChatArea* area) const
{
    Location location;
    if (!area) {
        return location;
    }

    location.channelId = area->getChannel().id;
    if (area->isThread) {
        location.rootId = area->root_id;
    }

    if (auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"))) {
        QString postId;
        if (log->captureViewportBookmark(postId)) {
            location.postId = postId;
            return location;
        }
    }

    location.postId = area->storedNavigationBookmark();
    return location;
}

NavigationTabsModel::Entry
NavigationUiController::tabEntry(const Location& location) const
{
    NavigationTabsModel::Entry entry;
    entry.channelId = location.channelId;
    entry.rootId = location.rootId;
    entry.postId = location.postId;
    entry.title = tabTitle(location);
    return entry;
}

NavigationUiController::Location
NavigationUiController::tabLocation(const NavigationTabsModel::Entry& entry) const
{
    Location location;
    location.channelId = entry.channelId;
    location.rootId = entry.rootId;
    location.postId = entry.postId;
    return location;
}

QString NavigationUiController::tabTitle(const Location& location) const
{
    QString title = location.channelId;
    if (Backend* sourceBackend = backend()) {
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
                                                bool deduplicate)
{
    if (!navigationTabs || !location.isValid()) {
        return -1;
    }

    const int oldCount = tabModel.count();
    const int index = tabModel.append(tabEntry(location), deduplicate);
    if (index < 0) {
        return -1;
    }

    const auto* entry = tabModel.at(index);
    if (index < oldCount && deduplicate) {
        navigationTabs->setTabText(index, entry ? entry->title : tabTitle(location));
    } else {
        navigationTabs->addTab(entry ? entry->title : tabTitle(location));
    }
    refreshTabBarVisibility();
    return index;
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
    if (!location.isValid()) {
        return;
    }

    const int index = appendNavigationTab(location, false);
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
    if (!navigationTabs || !location.isValid()
        || !tabModel.replace(index, tabEntry(location))) {
        return;
    }
    navigationTabs->setTabText(index, tabTitle(location));
}

void NavigationUiController::saveActiveTabLocation()
{
    const auto* entry = tabModel.at(activeTabIndex);
    if (!entry) {
        return;
    }

    ChatArea* area = nullptr;
    if (!entry->rootId.isEmpty()) {
        area = findThread(entry->channelId, entry->rootId);
    } else if (!navigationSurfaceStack
               || navigationSurfaceStack->currentWidget() == contentSplitter) {
        area = mainStack
            ? qobject_cast<ChatArea*>(mainStack->currentWidget())
            : nullptr;
    }

    if (!area) {
        return;
    }

    const Location location = captureLocation(area);
    const Location expected = tabLocation(*entry);
    if (location.isValid() && location.sameDestination(expected)) {
        updateTab(activeTabIndex, location);
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
    return tabModel.findDestination(area->getChannel().id, area->root_id);
}

void NavigationUiController::activateTab(int index)
{
    const auto* entry = tabModel.at(index);
    if (!entry || !navigationTabs) {
        return;
    }

    if (activeTabIndex != index) {
        saveActiveTabLocation();
    }

    const Location location = tabLocation(*entry);
    activeTabIndex = index;

    if (navigationTabs->currentIndex() != index) {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }

    const bool previousSwitching = switchingTabs;
    switchingTabs = true;

    if (!location.rootId.isEmpty()) {
        ChatArea* area = findThread(location.channelId, location.rootId);
        Backend* sourceBackend = backend();
        BackendChannel* channel = sourceBackend
            ? sourceBackend->getStorage().getChannelById(location.channelId)
            : nullptr;
        if (!area && sourceBackend && channel) {
            ChatArea* parent = channelTree ? channelTree->getCurrentPage() : nullptr;
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
                auto* log = area->findChild<ChatLogWidget*>(
                    QStringLiteral("listWidget"));
                if (!log || !log->restoreViewportBookmark(location.postId)) {
                    if (sourceBackend) {
                        AppNavigationService::instance(*sourceBackend)
                            .openPost(location.postId);
                    }
                }
            }
        }
    } else {
        if (navigationSurfaceStack && contentSplitter) {
            navigationSurfaceStack->setCurrentWidget(contentSplitter);
            contentSplitter->show();
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
        thread = findThread(removedEntry.channelId, removedEntry.rootId);
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
                    channelTree ? channelTree->getCurrentPage() : nullptr) {
                fallback = captureLocation(channelArea);
            }
            if (!fallback.isValid()) {
                fallback.channelId = removedEntry.channelId;
            }
            nextIndex = appendNavigationTab(fallback, false);
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
            threadStack->hide();
        }
    }

    if (area->isWindow()) {
        area->setWindowFlag(Qt::Window, false);
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

void NavigationUiController::openInTab(const QString& channelId,
                                       const QString& rootId,
                                       const QString& postId)
{
    Backend* sourceBackend = backend();
    BackendChannel* channel = sourceBackend
        ? sourceBackend->getStorage().getChannelById(channelId)
        : nullptr;
    if (!navigationTabs || !channel) {
        return;
    }

    ensureInitialTab();

    Location location;
    location.channelId = channelId;
    location.rootId = rootId;
    location.postId = postId;

    int index = -1;
    if (!rootId.isEmpty()) {
        index = tabModel.findDestination(channelId, rootId);
        if (index < 0) {
            index = appendNavigationTab(location, true);
        } else if (!postId.isEmpty()) {
            Location updated = tabLocation(*tabModel.at(index));
            updated.postId = postId;
            updateTab(index, updated);
        }
    } else {
        // Explicit channel "Open in new tab" is browser-like: even the same
        // destination can intentionally exist twice with independent bookmarks.
        index = appendNavigationTab(location, false);
    }

    if (index < 0) {
        return;
    }

    {
        QSignalBlocker blocker(navigationTabs);
        navigationTabs->setCurrentIndex(index);
    }
    activateTab(index);
}

void NavigationUiController::recordArea(ChatArea* area)
{
    if (!area) {
        return;
    }

    const Location next = captureLocation(area);
    if (!next.isValid()) {
        return;
    }

    if (navigationTabs && !switchingTabs) {
        if (area->isThread && area->property("threadTabbed").toBool()) {
            const int index = tabIndexForThread(area);
            if (index >= 0) {
                updateTab(index, next);
                activeTabIndex = index;
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(index);
            }
        } else if (!area->isThread) {
            if (tabModel.isEmpty()) {
                const int index = appendNavigationTab(next, false);
                if (index >= 0) {
                    activeTabIndex = index;
                    QSignalBlocker blocker(navigationTabs);
                    navigationTabs->setCurrentIndex(index);
                }
            } else {
                const auto* activeEntry = tabModel.at(activeTabIndex);
                if (activeEntry && activeEntry->rootId.isEmpty()) {
                    updateTab(activeTabIndex, next);
                } else {
                    int index = firstChannelTab();
                    if (index >= 0) {
                        updateTab(index, next);
                    } else {
                        index = appendNavigationTab(next, false);
                    }
                    if (index >= 0) {
                        activeTabIndex = index;
                        QSignalBlocker blocker(navigationTabs);
                        navigationTabs->setCurrentIndex(index);
                        if (navigationSurfaceStack && contentSplitter) {
                            navigationSurfaceStack->setCurrentWidget(contentSplitter);
                        }
                    }
                }
            }
        }
        refreshTabBarVisibility();
    }

    if (!currentLocation.isValid()) {
        currentLocation = next;
        activeArea = area;
        updateHistoryButtons();
        return;
    }

    if (currentLocation.sameDestination(next)) {
        currentLocation = next;
        activeArea = area;
        updateHistoryButtons();
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }

    if (!replayingHistory && currentLocation.isValid()) {
        backStack.push_back(currentLocation);
        trimHistory(backStack);
        forwardStack.clear();
    }

    currentLocation = next;
    activeArea = area;
    updateHistoryButtons();
}

void NavigationUiController::updateHistoryButtons()
{
    if (backButton) {
        backButton->setEnabled(!backStack.isEmpty());
    }
    if (forwardButton) {
        forwardButton->setEnabled(!forwardStack.isEmpty());
    }
}

void NavigationUiController::goBack()
{
    if (backStack.isEmpty()) {
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }
    if (currentLocation.isValid()) {
        forwardStack.push_back(currentLocation);
        trimHistory(forwardStack);
    }

    const Location target = backStack.takeLast();
    replayingHistory = true;
    navigateTo(target);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::goForward()
{
    if (forwardStack.isEmpty()) {
        return;
    }

    if (activeArea) {
        const Location latest = captureLocation(activeArea);
        if (latest.isValid()) {
            currentLocation = latest;
        }
    }
    if (currentLocation.isValid()) {
        backStack.push_back(currentLocation);
        trimHistory(backStack);
    }

    const Location target = forwardStack.takeLast();
    replayingHistory = true;
    navigateTo(target);
    replayingHistory = false;
    updateHistoryButtons();
}

void NavigationUiController::navigateTo(const Location& location)
{
    Backend* sourceBackend = backend();
    if (!sourceBackend || !channelTree || !location.isValid()) {
        return;
    }

    BackendChannel* channel = sourceBackend->getStorage().getChannelById(location.channelId);
    if (!channel) {
        return;
    }

    ChatArea* area = nullptr;
    if (location.rootId.isEmpty()) {
        channelTree->openStoredChannel(location.channelId);
        area = channelTree->getCurrentPage();
        if (!area || &area->getChannel() != channel) {
            return;
        }
        recordArea(area);
    } else {
        area = findThread(location.channelId, location.rootId);
        if (!area) {
            ChatArea* parent = channelTree->getCurrentPage();
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

    currentLocation = location;
    activeArea = area;
    if (location.postId.isEmpty() || !area) {
        return;
    }

    // A history entry whose semantic bookmark is still present can be restored
    // locally without a flash. If its source no longer knows the post, do not
    // start another ChatArea-level state machine: resolve and present it through
    // the same application navigation path used by every external post target.
    auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
    if (log && log->restoreViewportBookmark(location.postId)) {
        return;
    }

    AppNavigationService::instance(*sourceBackend).openPost(location.postId);
}

ChatArea* NavigationUiController::findThread(const QString& channelId,
                                             const QString& rootId) const
{
    if (channelId.isEmpty() || rootId.isEmpty()) {
        return nullptr;
    }

    const auto widgets = QApplication::allWidgets();
    for (QWidget* widget : widgets) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (!area || !area->isThread || area->root_id != rootId) {
            continue;
        }
        if (area->getChannel().id == channelId) {
            return area;
        }
    }
    return nullptr;
}

void NavigationUiController::ensureThreadButton(ChatArea* area)
{
    if (!area || !area->isThread) {
        return;
    }

    auto* layout = area->findChild<QHBoxLayout*>(QStringLiteral("propertieslLayout"));
    if (!layout) {
        return;
    }

    auto* tabButton = area->findChild<QToolButton*>(
        QStringLiteral("threadTabButton"));
    if (!tabButton) {
        tabButton = new QToolButton(area);
        tabButton->setObjectName(QStringLiteral("threadTabButton"));
        tabButton->setAutoRaise(true);
        tabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        tabButton->setIconSize(QSize(16, 16));
        tabButton->setCursor(Qt::PointingHandCursor);
        layout->addWidget(tabButton, 0, Qt::AlignVCenter);

        connect(tabButton, &QToolButton::clicked, this, [this, area] {
            if (!area) {
                return;
            }
            if (area->property("threadTabbed").toBool()) {
                attachThread(area);
                return;
            }
            if (Backend* sourceBackend = backend()) {
                AppNavigationService::instance(*sourceBackend).openThreadInTab(
                    area->getChannel().id, area->root_id);
            }
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
        layout->addWidget(presentationButton, 0, Qt::AlignVCenter);

        connect(presentationButton, &QToolButton::clicked, this, [this, area] {
            if (!area) {
                return;
            }
            if (area->property("threadDetached").toBool()) {
                attachThread(area);
            } else {
                detachThread(area);
            }
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
        layout->addWidget(closeButton, 0, Qt::AlignVCenter);

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
                    threadStack->hide();
                }
            }

            if (wasDockedCurrent) {
                syncSplitterEdgeGutters();
            }
            area->setSplitterEdgeGutters(false, false);
            area->close();
            if (wasActive) {
                QTimer::singleShot(0, this, [this] {
                    if (channelTree) {
                        recordArea(channelTree->getCurrentPage());
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

    if (area->property("threadTabbed").toBool()) {
        const int tabIndex = tabIndexForThread(area);
        if (tabIndex >= 0) {
            removeTab(tabIndex, false);
        }

        int channelTab = firstChannelTab();
        if (channelTab < 0) {
            Location channelLocation;
            channelLocation.channelId = area->getChannel().id;
            channelTab = appendNavigationTab(channelLocation, false);
        }
        if (channelTab >= 0) {
            {
                QSignalBlocker blocker(navigationTabs);
                navigationTabs->setCurrentIndex(channelTab);
            }
            activateTab(channelTab);
        }
    }

    if (auto* previous = qobject_cast<ChatArea*>(threadStack->currentWidget());
        previous && previous != area) {
        previous->setSplitterEdgeGutters(false, false);
    }

    area->hide();
    if (threadStack->indexOf(area) < 0) {
        area->setWindowFlag(Qt::Window, false);
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

    if (ensureThreadPaneExpanded(*contentSplitter, !threadSplitterStateRestored)) {
        threadSplitterStateRestored = true;
    }
    updateThreadButton(area);
    recordArea(area);
}

void NavigationUiController::detachThread(ChatArea* area)
{
    if (!area || !threadStack) {
        return;
    }

    if (area->property("threadTabbed").toBool()) {
        const int tabIndex = tabIndexForThread(area);
        if (tabIndex >= 0) {
            removeTab(tabIndex, false);
        }
    }

    const bool wasCurrent = threadStack->currentWidget() == area;
    area->hide();
    threadStack->removeWidget(area);

    area->setSplitterEdgeGutters(false, false);

    area->setParent(nullptr);
    area->setWindowFlag(Qt::Window, true);
    area->setAttribute(Qt::WA_DeleteOnClose, true);
    area->setProperty("threadDetached", true);
    area->setProperty("threadTabbed", false);
    updateThreadButton(area);

    if (wasCurrent) {
        threadStack->hide();
    }
    syncSplitterEdgeGutters();
    area->show();
    area->raise();
    area->activateWindow();
    recordArea(area);
}

void NavigationUiController::syncSplitterEdgeGutters()
{
    auto* channelArea = mainStack
        ? qobject_cast<ChatArea*>(mainStack->currentWidget())
        : nullptr;
    auto* threadArea = threadStack
        ? qobject_cast<ChatArea*>(threadStack->currentWidget())
        : nullptr;

    const bool normalSurfaceVisible = !navigationSurfaceStack
        || navigationSurfaceStack->currentWidget() == contentSplitter;
    const bool threadDocked = normalSurfaceVisible
        && threadStack && !threadStack->isHidden()
        && threadArea && !threadArea->property("threadDetached").toBool();

    if (channelArea) {
        // The main chat always touches the sidebar splitter on the left.
        // Its right edge touches the thread splitter only while that pane is open.
        channelArea->setSplitterEdgeGutters(true, threadDocked);
    }
    if (threadArea && threadDocked) {
        threadArea->setSplitterEdgeGutters(true, false);
    }
}

void NavigationUiController::presentChannel(ChatArea* area)
{
    if (!area || area->isThread) {
        return;
    }

    if (navigationSurfaceStack && contentSplitter) {
        navigationSurfaceStack->setCurrentWidget(contentSplitter);
        contentSplitter->show();
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

    attachThread(area);
}

bool NavigationUiController::eventFilter(QObject* watched, QEvent* event)
{
    const bool channelPointerSurface = channelTree
        && (watched == channelTree || watched == channelTree->viewport());
    if (channelPointerSurface && event
        && event->type() == QEvent::MouseButtonRelease) {
        QTimer::singleShot(0, this, [this] {
            if (channelTree) {
                recordArea(channelTree->getCurrentPage());
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