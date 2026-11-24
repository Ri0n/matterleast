/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "mainwindow.h"

#include <algorithm>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QDateTime>
#include <QKeySequence>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QDialog>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWindow>

#include "./ui_mainwindow.h"
#include "Settings.h"
#include "SettingsWindow.h"
#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
#include "update/WindowsUpdateController.h"
#include "update/UpdateTrustPolicy.h"
#include "ui/SidebarUpdateBanner.h"
#endif
#include "backend/Backend.h"
#include "backend/CustomStatusValidation.h"
#include "backend/PendingPostService.h"
#include "backend/FollowingModel.h"
#include "navigation/NavigationUiController.h"
#include "navigation/AppNavigationService.h"
#include "backend/AccountManager.h"
#include "backend/Account.h"
#include "ui/ServersSidebarPage.h"
#include "navigation/ConversationReference.h"
#include "backend/SidebarService.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "build-config.h"
#include "channel-tree/AttentionList.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/ChannelQuickList.h"
#include "channel-tree/SidebarUnreadPolicy.h"
#include "channel-tree-dialogs/FilterListDialog.h"
#include "channel-tree-dialogs/TeamChannelsListDialog.h"
#include "channel-tree-dialogs/UserSearchDialog.h"
#include "channel-tree-dialogs/UserGroupsDialog.h"
#include "chat-area/ChatArea.h"
#include "log.h"
#include "notifications/NotificationManager.h"
#include "options/MLOptions.h"
#include "post-collection/PostCollectionView.h"
#include "ui/IconUtils.h"
#include "ui/EmojiPresentation.h"
#include "ui/TeamSelectorLabel.h"
#include "ui/ThinSplitter.h"

namespace Mattermost {
namespace {

bool filterTreeItem(QTreeWidgetItem* item, const QString& term)
{
	if (!item) {
		return false;
	}

	bool childMatches = false;
	for (int i = 0; i < item->childCount(); ++i) {
		childMatches = filterTreeItem(item->child(i), term) || childMatches;
	}

	const bool ownMatch = term.isEmpty()
		|| item->text(0).contains(term, Qt::CaseInsensitive);
	const bool visible = ownMatch || childMatches;
	item->setHidden(!visible);
	return visible;
}

} // namespace

MainWindow::MainWindow(QWidget* parent, QSystemTrayIcon& trayIcon, Backend& source)
    : MainWindow(parent, trayIcon)
{
    attachInitialBackend(source);
}

MainWindow::MainWindow(QWidget* parent, QSystemTrayIcon& trayIcon)
    : QMainWindow(parent)
    , ui(std::make_unique<Ui::MainWindow>())
    , trayIcon(trayIcon)
    , notificationManager(&NotificationManager::instance(trayIcon))
    , currentTeamRestoredFromSettings(false)
    , doDeinit(false)
{
	LOG_DEBUG("MainWindow create start");

	ui->setupUi(this);
	ui->toolButton->installEventFilter(this);
    ui->searchButton->installEventFilter(this);
	setupChannelTabs();
	refreshMenuButtonIcon();
    refreshSearchButtonIcon();
    connect(ui->searchButton, &QToolButton::clicked,
            this, &MainWindow::openMessageSearch);
    connect(ui->channelList, &ChannelTree::virtualDestinationRequestedForAccount,
            this, [this](const QString& accountId, int destination, const QString& teamId) {
        auto* source = navigationBackend(accountId);
        if (!source) return;
        if (backendOrNull() != source) emit serverOpenRequested(accountId);
        if (backendOrNull() == source) openAccountDestination(destination, teamId);
    });
    connect(ui->channelList, &ChannelTree::virtualDestinationRequested,
            this, [this](int destination, const QString& teamId) {
        if (destination == SidebarItem::SavedDestination) {
            openSavedMessages(teamId);
        } else if (destination == SidebarItem::DraftsDestination) {
            openDrafts(teamId);
        }
    });
    if (auto* delegate = qobject_cast<ChannelItemDelegate*>(
            ui->channelList->itemDelegate())) {
        connect(delegate, &ChannelItemDelegate::categoryActionRequested,
                this, [this](const QString& accountId, const QString& teamId,
                             const QString& categoryId) {
            auto* owner = ui->channelList->backendForAccount(accountId);
            if (!owner) return;
            const SidebarTeamState* state =
                SidebarService::instance(*owner).teamState(teamId);
            const SidebarCategory* category = state ? state->category(categoryId) : nullptr;
            if (!category) {
                return;
            }
            if (category->type == QStringLiteral("direct_messages")) {
                UserSearchDialog::showConversationPicker(*owner, this);
            } else if (category->type == QStringLiteral("channels")) {
                if (BackendTeam* team = owner->getStorage().getTeamById(teamId)) {
                    TeamChannelsListDialog::showForTeam(
                        *owner, *team, ui->channelList);
                }
            }
        });
    }

	ui->channelList->setChatAreaStackedWidget(ui->chatAreaStackedWidget);
	ui->channelList->setFocus();

	createMenu();

	connect(ui->channelList, &QTreeWidget::currentItemChanged, this,
	        [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (_sharedPresentation && current
            && current->data(0, SidebarItem::KindRole).toInt() == SidebarItem::AccountRoot) {
            emit serverOpenRequested(current->data(0, SidebarItem::AccountIdRole).toString());
            return;
        }
		if (!current
			|| current->data(0, ChannelTree::ItemKindRole).toInt()
				!= ChannelTree::ChannelItemKind) {
			return;
		}

		const QString channelId = current->data(0, ChannelTree::ItemIdRole).toString();
		if (channelId.isEmpty()) {
			return;
		}

		const bool channelsTabVisible = channelTabs && channelsPage
			&& channelTabs->currentWidget() == channelsPage;
		if (channelsTabVisible && unreadFilterButton && unreadFilterButton->isChecked()) {
			retainedUnreadFilterChannelId = channelId;
            retainedUnreadFilterAccountId = current->data(0, SidebarItem::AccountIdRole).toString();
			QTimer::singleShot(0, this, &MainWindow::refreshChannelUnreadFilter);
		} else if (channelsTabVisible) {
			retainedUnreadFilterChannelId.clear();
		}
	});

    // These widgets belong to the window and are created even when every
    // account is offline. Session attachment only enables their actions.
    _defaultProfileStatusFont = ui->statusLabel->font();
    ui->usericon_label->clear();
    ui->usericon_label->setEnabled(false);
    connect(ui->usericon_label, &PresenceAvatarLabel::reconnectRequested, this, [this] {
        if (_profileBackend) _profileBackend->retryWebSocketConnectionNow();
    });
    ui->searchButton->setEnabled(false);
    if (auto* action = mainMenu->findChild<QAction*>(QStringLiteral("sidebarLogoutAction")))
        action->setEnabled(false);
    if (auto* action = mainMenu->findChild<QAction*>(QStringLiteral("sidebarUserGroupsAction")))
        action->setEnabled(false);

    if (sidebarSplitter) {
        sidebarSplitter->setSizes({280, std::max(360, width() - 280)});
        sidebarSplitter->setHandleWidth(ThinSplitter::VisibleHandleExtent);
        connect(sidebarSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
            if (!sidebarSplitter->isVisible()) return;
            const auto sizes = sidebarSplitter->sizes();
            if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 1)
                MLOptions::instance()->setValue(QStringLiteral("sidebar_splitter_state"),
                                                 sidebarSplitter->saveState());
        });
    }
    connect(qApp, &QApplication::aboutToQuit, this, &MainWindow::saveState);
}

Backend* MainWindow::backendOrNull() const
{
    return _backend.data();
}

Backend& MainWindow::backendInstance() const
{
    Q_ASSERT(_backend);
    return *_backend.data();
}

bool MainWindow::attachInitialBackend(Backend& source)
{
    if (_backend)
        return _backend.data() == &source;
    const BackendUser* user = source.getStorage().loginUser;
    if (!user || user->id.isEmpty())
        return false;
    _backend = &source;
    setProfileBackend(&source);
    ui->searchButton->setEnabled(true);
    if (auto* action = mainMenu->findChild<QAction*>(QStringLiteral("sidebarLogoutAction")))
        action->setEnabled(true);
    if (auto* action = mainMenu->findChild<QAction*>(QStringLiteral("sidebarUserGroupsAction"))) {
        action->setEnabled(true);
    }

    // Standalone widget fixtures also use the account's idempotent startup
    // path. The production AccountManager has already started it.
    source.startAccountData();
    auto& sidebar = SidebarService::instance(source);
    connect(&sidebar, &SidebarService::channelActivityChanged, this,
            [this](const QString&) { refreshSidebarViews(); });
    connect(&sidebar, &SidebarService::channelActivityReset,
            this, &MainWindow::refreshSidebarViews);
    connect(&sidebar, &SidebarService::categoriesChanged, this,
            [this](const QString&) {
        QTimer::singleShot(0, this, &MainWindow::refreshSidebarViews);
    });

    recentChannels->initialize(backendInstance());
    attentionList->initialize(backendInstance());
    installRealtimeUiSync();

    QPointer<MainWindow> windowGuard(this);
    sidebar.retrieveChannelMemberships([windowGuard] {
        if (!windowGuard) return;
        windowGuard->channelMembershipsReady = true;
        windowGuard->restoreNavigationSessionWhenReady();
    });
    connect(ui->channelList, &ChannelTree::teamSidebarPopulated, this,
        [this](const QString& teamId) {
            populatedSessionSidebars.insert(teamId);
            restoreNavigationSessionWhenReady();
        });


    const auto addTeam = [this, &source](BackendTeam& team) {
        ui->channelList->addTeam(source, team);
    };
    connect(&source, &Backend::ownTeamDiscovered, this, addTeam);
    connect(&source, &Backend::onAddedToTeam, this, addTeam);
    const auto synchronize = [this, &source] {
        ui->channelList->synchronizeAccountSidebar(ui->channelList->accountIdForBackend(&source));
        refreshSidebarViews();
        initializationComplete();
    };
    connect(&source, &Backend::onAllTeamChannelsPopulated, this, synchronize);
    for (auto& team : source.getStorage().teams)
        addTeam(team.second);
    ui->channelList->observeAccountSidebar(source);
    if (source.accountCatalogReady()) {
        synchronize();
    }

    LOG_DEBUG("MainWindow signal register finish");

    // Register the Backend-owned navigation receiver once the complete
    // channel/thread presentation exists, before plugins can navigate.
    AppNavigationService::instance(source).bindMainWindow(*this);
    if (auto* navigation = findChild<NavigationUiController*>(
            QStringLiteral("navigationUiController"), Qt::FindDirectChildrenOnly))
        navigation->attachBackend(source);
    LOG_DEBUG("MainWindow account attached");
    return true;
}

Backend* MainWindow::navigationBackend(const QString& accountId) const
{
    if (auto* owner = ui->channelList->backendForAccount(accountId))
        return owner;
    // Existing standalone widgets have no Account object. Preserve their
    // private semantic identity without inventing an ID for another session.
    if (auto* primary = backendOrNull()) {
        const auto* account = qobject_cast<const Account*>(primary->parent());
        if ((!account && accountId == QStringLiteral("_startup"))
            || (account && account->id() == accountId))
            return primary;
    }
    return nullptr;
}

bool MainWindow::admitsNavigationBackend(const Backend& source) const
{
    if (&source == backendOrNull()) return true;
    const auto* account = qobject_cast<const Account*>(source.parent());
    return account && navigationBackend(account->id()) == &source;
}

bool MainWindow::attachNavigationBackend(Backend& source)
{
    if (!_sharedPresentation && &source == backendOrNull()) return true;
    const auto* account = qobject_cast<const Account*>(source.parent());
    if (!account || !source.getStorage().loginUser) return false;
    if (!_sharedPresentation && backendOrNull()) {
        auto* primary = backendOrNull();
        const auto* primaryAccount = qobject_cast<const Account*>(primary->parent());
        if (!primaryAccount) return false;
        if (!ui->channelList->bindSingleAccount(primaryAccount->id())
            || !ui->channelList->registerAccountBackend(primaryAccount->id(), *primary))
            return false;
    }
    if (!ui->channelList->registerAccountBackend(account->id(), source)) return false;
    auto& navigation = NavigationUiController::instance(*this);
    navigation.attachBackend(source);
    AppNavigationService::instance(source).bindMainWindow(*this);
    if (!_navigationSubscriptions.contains(&source)) {
        auto* context = new QObject(this);
        _navigationSubscriptions.insert(&source, context);
        const QString accountId = account->id();
        connect(&source, &QObject::destroyed, context, [this, key = &source] {
            delete _navigationSubscriptions.take(key);
        });
        auto& sidebar = SidebarService::instance(source);
        connect(&sidebar, &SidebarService::channelActivityChanged, context,
                [this] { refreshSidebarViews(); });
        connect(&sidebar, &SidebarService::channelActivityReset, context,
                [this] { refreshSidebarViews(); });
        connect(&sidebar, &SidebarService::categoriesChanged, context,
                [this, &source, accountId] {
            if (ui->channelList->backendForAccount(accountId) != &source) return;
            refreshSidebarViews();
            NavigationUiController::instance(*this).refreshAttachedDestination(source);
        });
        connect(&source, &Backend::ownTeamDiscovered, context,
                [this, &source](BackendTeam& team) { ui->channelList->addTeam(source, team); });
        connect(&source, &Backend::onAddedToTeam, context,
                [this, &source](BackendTeam& team) { ui->channelList->addTeam(source, team); });
        connect(&source, &Backend::onAllTeamChannelsPopulated, context,
                [this, &source, accountId] {
            ui->channelList->synchronizeAccountSidebar(accountId);
            NavigationUiController::instance(*this).refreshAttachedDestination(source);
        });
        installRealtimeUiSync(source, context);
        ui->channelList->synchronizeAccountSidebar(accountId);
        ui->channelList->observeAccountSidebar(source);
    }
    refreshQueueAccounts();
    return true;
}

void MainWindow::detachNavigationBackend(Backend& source)
{
    if (!admitsNavigationBackend(source)) return;
    beginSemanticNavigation();
    const QString accountId = ui->channelList->accountIdForBackend(&source);
    delete _navigationSubscriptions.take(&source);
    if (_profileBackend == &source) setProfileBackend(nullptr);
    NavigationUiController::instance(*this).detachBackend(source);
    ui->channelList->unregisterAccountBackend(accountId, &source);
    if (_backend == &source) {
        if (_sharedPresentation) selectPresentationBackend(nullptr);
        else _backend.clear();
    }
    refreshQueueAccounts();
    refreshSidebarViews();
}

void MainWindow::enableSharedPresentation()
{
    if (_sharedPresentation) return;
    _sharedPresentation = true;
    // Restore the application split only after navigation installs its final
    // content host. The empty pre-authentication stack has no size hint.
    NavigationUiController::instance(*this);
    if (sidebarSplitter) {
        const QByteArray state = MLOptions::instance()->value<QByteArray>(
            QStringLiteral("sidebar_splitter_state"));
        if (!state.isEmpty()) sidebarSplitter->restoreState(state);
        sidebarSplitter->setChildrenCollapsible(false);
        sidebarSplitter->setHandleWidth(ThinSplitter::VisibleHandleExtent);
        const auto sizes = sidebarSplitter->sizes();
        if (sizes.size() == 2 && (sizes[0] <= 0 || sizes[1] <= 1))
            sidebarSplitter->setSizes({280, std::max(360, width() - 280)});
    }
}

void MainWindow::selectPresentationBackend(Backend* source)
{
    if (source && !admitsNavigationBackend(*source)) return;
    _backend = source;
    if (_profileBackend != source) setProfileBackend(source);
    ui->searchButton->setEnabled(source);
    if (auto* action = mainMenu->findChild<QAction*>(QStringLiteral("sidebarLogoutAction")))
        action->setEnabled(source);
    if (auto* action = mainMenu->findChild<QAction*>(QStringLiteral("sidebarUserGroupsAction")))
        action->setEnabled(source);
}

void MainWindow::setVisibleAccounts(const QStringList& ids,
                                    const QHash<QString, QString>& labels,
                                    const QString& sorting)
{
    if (ui->channelList->visibleAccountRoots() != ids)
        ui->channelList->setRootIndex(QModelIndex());
    ui->channelList->setVisibleAccountRoots(ids, labels);
    ui->channelList->setMergedConversationSorting(sorting);
    ui->teamSelectorLabel->refreshPresentation();
    ui->channelList->setConversationSearchTerm(sidebarFilterEdit->text());
    refreshSidebarViews();
}

void MainWindow::showStartupError(const QString& message)
{
    showServersSidebar();
    auto* dialog = new QMessageBox(QMessageBox::Warning, tr("Account settings"),
                                   message, QMessageBox::Ok, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

MainWindow::~MainWindow()
{
    // QObject destroys child widgets only after this destructor body returns.
    // Finalize navigation now while every thread stack and ChatArea is valid;
    // otherwise ChatArea::destroyed synchronously triggers session updates
    // against partially destroyed QStackedWidgets (startup/exit SIGSEGV).
    if (auto* navigation = findChild<NavigationUiController*>(
            QStringLiteral("navigationUiController"), Qt::FindDirectChildrenOnly))
        navigation->beginShutdown();
}

void MainWindow::bindViewNavigation(ViewNavigationSession* session,
                                    const QString& accountId)
{
    if (backendOrNull() || accountId.isEmpty()) {
        NavigationUiController::instance(*this).bindViewSession(session, accountId);
        refreshQueueAccounts();
    }
}

void MainWindow::unbindViewNavigation()
{
    NavigationUiController::instance(*this).unbindViewSession();
    refreshQueueAccounts();
}

void MainWindow::refreshQueueAccounts()
{
    SidebarQueueSources::Accounts sources;
    for (auto* owner : NavigationUiController::instance(*this).visibleBackends())
        sources.insert(SidebarQueueSources::accountId(*owner), owner);
    recentChannels->setAccounts(sources);
    attentionList->setAccounts(sources);
    applySidebarTextFilter(recentChannels);
    applySidebarTextFilter(attentionList);
}

void MainWindow::installServersSidebar(AccountManager& manager)
{
    if (!channelTabs || serversPage) return;
    if (const Account* owner = manager.accountForBackend(backendOrNull()))
        ui->channelList->bindSingleAccount(owner->id());
    serversPage = new ServersSidebarPage(manager, channelTabs);
    // One icon, no caption, immediately before Channels. The server manager
    // is a navigation surface, not another button in the avatar header.
    const int index = channelTabs->insertTab(0, serversPage,
        style()->standardIcon(QStyle::SP_ComputerIcon), QString());
    channelTabs->setTabToolTip(index, tr("Servers and views"));
    channelTabs->setTabWhatsThis(index, tr("Edit server settings and workspace layout"));
    connect(serversPage, &ServersSidebarPage::accountOpenRequested,
            this, &MainWindow::serverOpenRequested);
    connect(serversPage, &ServersSidebarPage::viewOpenRequested,
            this, &MainWindow::viewOpenRequested);
    connect(serversPage, &ServersSidebarPage::addServerRequested,
            this, &MainWindow::addServerRequested);
    connect(serversPage, &ServersSidebarPage::serverSettingsRequested,
            this, &MainWindow::serverSettingsRequested);
    connect(serversPage, &ServersSidebarPage::disconnectRequested,
            this, &MainWindow::serverDisconnectRequested);
    connect(serversPage, &ServersSidebarPage::retryRequested,
            this, &MainWindow::serverConnectRequested);
    connect(channelTabs, &QTabWidget::currentChanged, this, [this] {
        if (sidebarFilterRow)
            sidebarFilterRow->setEnabled(channelTabs->currentWidget() != serversPage);
    });
}

void MainWindow::showServersSidebar()
{
    if (channelTabs && serversPage)
        channelTabs->setCurrentWidget(serversPage);
}

void MainWindow::showChannelsSidebar()
{
    if (channelTabs && channelsPage)
        channelTabs->setCurrentWidget(channelsPage);
}

void MainWindow::openAccountDestination(int destination, const QString& teamId)
{
    if (destination == SidebarItem::SavedDestination)
        openSavedMessages(teamId);
    else if (destination == SidebarItem::DraftsDestination)
        openDrafts(teamId);
    else if (destination == SidebarItem::RecentMentionsDestination)
        on_channelList_virtualDestinationRequested(destination, teamId);
}

static QString infoText()
{
    QString description = QString("MatterLeast " PROJECT_VER "<br/>"
                                 "An unofficial native Mattermost client using the Qt framework.<br/>") +
R"(
<br/>
MatterLeast development and maintenance:<br/>
<a href='https://github.com/Ri0n'>Sergei Ilinykh (Ri0n)</a><br/>
<br/>
Based on Mattermost-QT by Lyubomir Filipov, Copyright 2021, 2022.<br/>
Further development of the predecessor fork by Maksim Turok.<br/>
<br/>
More information:<br/>
<a href='https://github.com/Ri0n/matterleast'>https://github.com/Ri0n/matterleast</a>
<br/>
<br/>
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.<br/>
<br/>
You should have received a copy of the GNU Lesser General Public License
along with MatterLeast. If not, see <a href='https://www.gnu.org/licenses/'>https://www.gnu.org/licenses/</a>.<br/>
)";
    const QString buildId = qApp->property("matterleast-build-id").toString();
    if (!buildId.isEmpty()) {
        description.insert(description.indexOf(QStringLiteral("<br/>")),
                           QStringLiteral("<br/><b>Build:</b> %1")
                               .arg(buildId.toHtmlEscaped()));
    }
    return description;
}

void MainWindow::setupChannelTabs()
{
	ui->gridLayout_2->removeWidget(ui->lefttop_frame);
	ui->gridLayout_2->removeWidget(ui->channelList);
	ui->gridLayout_2->removeWidget(ui->chatAreaStackedWidget);

	channelTabs = new QTabWidget(ui->centralwidget);
	channelTabs->setDocumentMode(true);
	channelTabs->setSizePolicy(ui->channelList->sizePolicy());

	channelsPage = new QWidget(channelTabs);
	auto* channelsLayout = new QVBoxLayout(channelsPage);
	channelsLayout->setContentsMargins(0, 0, 0, 0);
	channelsLayout->setSpacing(0);
	ui->teamSelectorLabel->hide();
	unreadFilterButton = new QToolButton(channelsPage);
	unreadFilterButton->setObjectName(QStringLiteral("channelsUnreadFilterButton"));
	unreadFilterButton->setCheckable(true);
	unreadFilterButton->setAutoRaise(true);
	unreadFilterButton->setStyleSheet(QStringLiteral(
		"QToolButton:checked { background-color: palette(highlight); "
		"color: palette(highlighted-text); border: none; border-radius: 4px; }"));
	unreadFilterButton->setFixedSize(28, 28);
	unreadFilterButton->setIconSize(QSize(20, 20));
	unreadFilterButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	unreadFilterButton->setToolTip(tr("Show unread only"));
	unreadFilterButton->setAccessibleName(tr("Show unread only"));
	unreadFilterButton->installEventFilter(this);
	refreshUnreadFilterIcon();
	QWidget* teamToolbar =
        ui->teamSelectorLabel->createTeamToolbar(channelsPage, unreadFilterButton);
    // The toolbar itself receives palette updates even when a button's
    // stylesheet prevents it from inheriting the new palette correctly.
    teamToolbar->installEventFilter(this);
    channelsLayout->addWidget(teamToolbar);
    channelsLayout->addWidget(ui->channelList, 1);

    // Reuse the existing Following projection inside an Unread tab. The
    // filters remain local view preferences, not new unread state.
    _unreadPage = new QWidget(channelTabs);
    _unreadPage->setObjectName(QStringLiteral("unreadSidebarPage"));
    auto* unreadLayout = new QVBoxLayout(_unreadPage);
    unreadLayout->setContentsMargins(0, 0, 0, 0);
    unreadLayout->setSpacing(0);
    auto* unreadFilterRow = new QWidget(_unreadPage);
    auto* unreadFilterLayout = new QHBoxLayout(unreadFilterRow);
    unreadFilterLayout->setContentsMargins(4, 2, 3, 2);
    unreadFilterLayout->setSpacing(4);
    _unreadFollowingCheck = new QCheckBox(tr("Following"), unreadFilterRow);
    _unreadFollowingCheck->setObjectName(QStringLiteral("unreadFollowingFilter"));
    _unreadFollowingCheck->setToolTip(
        tr("Direct messages, mentions and followed threads (including read threads)"));
    _unreadChannelsCheck = new QCheckBox(tr("Channels"), unreadFilterRow);
    _unreadChannelsCheck->setObjectName(QStringLiteral("unreadChannelsFilter"));
    _unreadChannelsCheck->setToolTip(
        tr("Unread public and private channels"));
    _unreadMutedCheck = new QCheckBox(tr("Muted"), unreadFilterRow);
    _unreadMutedCheck->setObjectName(QStringLiteral("unreadMutedFilter"));
    _unreadMutedCheck->setToolTip(tr("Include muted channels"));
    unreadFilterLayout->addWidget(_unreadFollowingCheck);
    unreadFilterLayout->addWidget(_unreadChannelsCheck);
    unreadFilterLayout->addWidget(_unreadMutedCheck);
    unreadFilterLayout->addStretch();
    unreadLayout->addWidget(unreadFilterRow);

    auto* unreadPreferences = MLOptions::instance();
    _unreadFollowingCheck->setChecked(unreadPreferences->value<bool>(
        UNREAD_VIEW_FOLLOWING, UNREAD_VIEW_FOLLOWING_DEFAULT));
    _unreadChannelsCheck->setChecked(unreadPreferences->value<bool>(
        UNREAD_VIEW_CHANNELS, UNREAD_VIEW_CHANNELS_DEFAULT));
    _unreadMutedCheck->setChecked(unreadPreferences->value<bool>(
        UNREAD_VIEW_MUTED, UNREAD_VIEW_MUTED_DEFAULT));

    recentChannels = new ChannelQuickList(_unreadPage);
    recentChannels->setObjectName(QStringLiteral("unreadSidebarList"));
    unreadLayout->addWidget(recentChannels, 1);
    const auto applyUnreadFilters = [this] {
        recentChannels->setUnreadFilters({
            _unreadFollowingCheck->isChecked(),
            _unreadChannelsCheck->isChecked(),
            _unreadMutedCheck->isChecked()
        });
        // Changing a source must not briefly reveal rows excluded by an
        // already-active sidebar text filter.
        applySidebarTextFilter(recentChannels);
    };
    connect(_unreadFollowingCheck, &QCheckBox::toggled, this,
            [unreadPreferences, applyUnreadFilters](bool checked) {
        unreadPreferences->setValue(UNREAD_VIEW_FOLLOWING, checked);
        applyUnreadFilters();
    });
    connect(_unreadChannelsCheck, &QCheckBox::toggled, this,
            [unreadPreferences, applyUnreadFilters](bool checked) {
        unreadPreferences->setValue(UNREAD_VIEW_CHANNELS, checked);
        applyUnreadFilters();
    });
    connect(_unreadMutedCheck, &QCheckBox::toggled, this,
            [unreadPreferences, applyUnreadFilters](bool checked) {
        unreadPreferences->setValue(UNREAD_VIEW_MUTED, checked);
        applyUnreadFilters();
    });
    applyUnreadFilters();

    attentionList = new AttentionList(channelTabs);
	attentionList->setContextMenuPolicy(Qt::CustomContextMenu);

	channelTabs->addTab(channelsPage, tr("Channels"));
	channelTabs->addTab(_unreadPage, tr("Unread"));
    channelTabs->setTabToolTip(channelTabs->indexOf(_unreadPage),
                              tr("Unread channels and followed conversations"));
	channelTabs->addTab(attentionList, tr("Attention"));

	auto* leftSidebar = new QWidget(ui->centralwidget);
	auto* leftLayout = new QVBoxLayout(leftSidebar);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	leftLayout->setSpacing(0);
	leftLayout->addWidget(ui->lefttop_frame);
	sidebarFilterRow = new QWidget(leftSidebar);
	auto* sidebarFilterLayout = new QHBoxLayout(sidebarFilterRow);
	sidebarFilterLayout->setContentsMargins(0, 0, 3, 0);
	sidebarFilterLayout->setSpacing(3);

	sidebarFilterEdit = new QLineEdit(sidebarFilterRow);
	sidebarFilterEdit->setClearButtonEnabled(true);
	sidebarFilterEdit->setPlaceholderText(tr("Filter channels or contacts…"));
	sidebarFilterEdit->setAccessibleName(tr("Filter channels or contacts"));
	sidebarFilterEdit->setContentsMargins(4, 2, 4, 2);

	sidebarFilterLayout->addWidget(sidebarFilterEdit, 1);
	leftLayout->addWidget(sidebarFilterRow);
	leftLayout->addWidget(channelTabs, 1);

#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
    // One persistent footer shared by Channels, Unread and Attention.
    // The updater belongs to the application and can discover updates before
    // the main window exists; initialize from its current state as well.
    if (auto* updater = qApp->findChild<WindowsUpdateController*>()) {
        _updateBanner = new SidebarUpdateBanner(leftSidebar);
        leftLayout->addWidget(_updateBanner);

        const auto refreshBanner = [this, updater] {
            if (!_updateBanner) return;
            using State = WindowsUpdateController::State;
            using Phase = SidebarUpdateBanner::Phase;
            const State state = updater->state();

            switch (state) {
            case State::Downloading: {
                _updateOfferVisible = true;
                const qint64 total = updater->downloadTotalBytes();
                const int percentage = total > 0
                    ? int(100 * updater->downloadedBytes() / total) : -1;
                _updateBanner->showUpdate(Phase::Downloading,
                    updater->availableVersion(), percentage, _updateRestartRequested);
                break;
            }
            case State::Preparing:
                _updateOfferVisible = true;
                _updateBanner->showUpdate(Phase::Preparing,
                    updater->availableVersion(), -1, _updateRestartRequested);
                break;
            case State::Ready:
                _updateOfferVisible = true;
                _updateBanner->showUpdate(Phase::Ready,
                    updater->availableVersion());
                if (_updateRestartRequested) {
                    _updateRestartRequested = false;
                    // Do not destroy the running app from a stateChanged()
                    // callback; wait until preparation has finished emitting.
                    QTimer::singleShot(0, updater, [updater] {
                        if (updater->canApply())
                            updater->startUpdateAndRestart();
                    });
                }
                break;
            case State::Failed:
                _updateRestartRequested = false;
                if (_updateOfferVisible) {
                    _updateBanner->showUpdate(Phase::Failed,
                        updater->availableVersion(), -1, false, updater->lastError());
                } else {
                    _updateBanner->clearUpdate();
                }
                break;
            case State::Unavailable:
            case State::Idle:
            case State::Checking:
                _updateOfferVisible = false;
                _updateRestartRequested = false;
                _updateBanner->clearUpdate();
                break;
            }
        };

        connect(_updateBanner, &SidebarUpdateBanner::activated, this,
                [this, updater, refreshBanner] {
            using State = WindowsUpdateController::State;
            if (updater->canApply()) {
                updater->startUpdateAndRestart();
            } else if (updater->state() == State::Downloading
                       || updater->state() == State::Preparing) {
                _updateRestartRequested = true;
                refreshBanner();
            } else if (updater->state() == State::Failed) {
                updater->checkNow();
            }
        });
        connect(updater, &WindowsUpdateController::stateChanged, this,
                [refreshBanner](WindowsUpdateController::State) { refreshBanner(); });
        connect(updater, &WindowsUpdateController::progressChanged, this,
                [refreshBanner](qint64, qint64) { refreshBanner(); });
        refreshBanner();
    }
#endif

	sidebarSplitter = new ThinSplitter(Qt::Horizontal, ui->centralwidget);
	sidebarSplitter->setChildrenCollapsible(false);
	sidebarSplitter->setOpaqueResize(true);
	sidebarSplitter->addWidget(leftSidebar);
	sidebarSplitter->addWidget(ui->chatAreaStackedWidget);
	sidebarSplitter->setStretchFactor(0, 0);
	sidebarSplitter->setStretchFactor(1, 1);
	ui->gridLayout_2->addWidget(sidebarSplitter, 0, 0, 2, 2);
	ui->gridLayout_2->setRowStretch(0, 1);
	ui->gridLayout_2->setRowStretch(1, 0);
	ui->gridLayout_2->setColumnStretch(0, 1);

	connect(sidebarFilterEdit, &QLineEdit::textChanged,
            ui->channelList, &ChannelTree::setConversationSearchTerm);
	connect(sidebarFilterEdit, &QLineEdit::textChanged,
	        this, &MainWindow::refreshSidebarViews);
	connect(unreadFilterButton, &QToolButton::toggled, this, [this] {
		refreshUnreadFilterIcon();
		refreshChannelUnreadFilter();
	});

    auto* ignoreFilteringOption = MLOptions::instance()->optionObject<bool>(
        UNREAD_MODE_IGNORE_WHILE_FILTERING,
        UNREAD_MODE_IGNORE_WHILE_FILTERING_DEFAULT);
    connect(ignoreFilteringOption, &MLOptionObject::changed, this,
            [this](const QVariant&) { refreshChannelUnreadFilter(); });

    connect(recentChannels, &ChannelQuickList::channelSelected, this,
            [this](const QString& channelId, Backend* owner) {
        if (owner)
            ui->channelList->openStoredChannelForAccount(
                channelId, ui->channelList->accountIdForBackend(owner));
    });
    connect(recentChannels, &ChannelQuickList::channelContextMenuRequested, this,
            [this](const QString& channelId, const QPoint& pos, Backend* owner) {
        if (owner)
            ui->channelList->showChannelContextMenuForAccount(
                channelId, ui->channelList->accountIdForBackend(owner), pos);
    });
    connect(attentionList, &AttentionList::conversationAttentionReactivated,
            this, [this](const QString& channelId, Backend* owner) {
        ChatArea* active = ui->channelList->getCurrentPage();
        if (active && !active->isThread && &active->getBackend() == owner
            && active->getChannel().id == channelId)
            active->flashVisibleAttention();
    });
    connect(attentionList, &AttentionList::channelSelected, this,
            [this](const QString& channelId, Backend* owner) {
        if (owner)
            ui->channelList->openStoredChannelForAccount(
                channelId, ui->channelList->accountIdForBackend(owner));
    });
	connect(attentionList, &QTreeWidget::customContextMenuRequested, this,
	        [this](const QPoint& pos) {
		const QString channelId = attentionList->channelIdAt(pos);
        if (Backend* owner = attentionList->backendAt(pos); owner && !channelId.isEmpty()) {
            ui->channelList->showChannelContextMenuForAccount(
                channelId, ui->channelList->accountIdForBackend(owner),
                attentionList->viewport()->mapToGlobal(pos));
        }
	});
	connect(attentionList, &AttentionList::attentionCountChanged, this,
	        [this](uint32_t count) {
		setNotificationsCountVisualization(count);
		const int index = channelTabs ? channelTabs->indexOf(attentionList) : -1;
		if (index < 0) {
			return;
		}

		const QString label = count == 0
			? tr("Attention")
			: tr("Attention") + QStringLiteral(" (%1)").arg(count);
		channelTabs->setTabText(index, label);

		if (count == 0) {
			channelTabs->setTabIcon(index, QIcon());
			channelTabs->setTabToolTip(index, QString());
			return;
		}

		QIcon icon = QIcon::fromTheme(QStringLiteral("mail-unread"));
		if (icon.isNull()) {
			icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
		}
		channelTabs->setTabIcon(index, icon);
		channelTabs->setTabToolTip(index,
			tr("%1 item(s) need your attention").arg(count));
	});

	connect(channelTabs, &QTabWidget::currentChanged, this, [this](int index) {
		QWidget* currentPage = channelTabs->widget(index);
		if (attentionList && currentPage != attentionList) {
			attentionList->releaseSelectionRetention();
		}
        if (recentChannels && currentPage != _unreadPage) {
            recentChannels->releaseSelectionRetention();
        }
		if (currentPage != channelsPage && !retainedUnreadFilterChannelId.isEmpty()) {
			retainedUnreadFilterChannelId.clear();
			refreshChannelUnreadFilter();
		}
		if (attentionList && currentPage == attentionList) {
			attentionList->refresh();
			attentionList->refreshThreads();
		}
		refreshSidebarViews();
	});
}

void MainWindow::refreshSidebarViews()
{
	if (recentChannels) {
		recentChannels->refresh();
		applySidebarTextFilter(recentChannels);
	}
	if (attentionList) {
		attentionList->refresh();
		applySidebarTextFilter(attentionList);
	}
	refreshChannelUnreadFilter();
}

void MainWindow::applySidebarTextFilter(QTreeWidget* tree) const
{
	if (!tree) {
		return;
	}

	const QString term = sidebarFilterEdit ? sidebarFilterEdit->text().trimmed() : QString();
	for (int i = 0; i < tree->topLevelItemCount(); ++i) {
		filterTreeItem(tree->topLevelItem(i), term);
	}
}

void MainWindow::refreshChannelUnreadFilter()
{
    if (!ui || !ui->channelList || !unreadFilterButton) return;

    const bool unreadOnly = unreadFilterButton->isChecked();
    auto* options = MLOptions::instance();
    const QString filterText = sidebarFilterEdit ? sidebarFilterEdit->text().trimmed() : QString();
    const bool textFilterActive = !filterText.isEmpty();
    const bool ignoreWhileFiltering = options->value<bool>(
        UNREAD_MODE_IGNORE_WHILE_FILTERING, UNREAD_MODE_IGNORE_WHILE_FILTERING_DEFAULT);
    const bool unreadGateActive = SidebarUnreadPolicy::unreadGateActive(
        unreadOnly, textFilterActive, ignoreWhileFiltering);
    const bool anyFilterActive = SidebarUnreadPolicy::anyFilterActive(
        unreadOnly, textFilterActive, ignoreWhileFiltering);

    if (!unreadOnly) {
        retainedUnreadFilterChannelId.clear();
        retainedUnreadFilterAccountId.clear();
    }
    const bool channelsTabVisible = channelTabs && channelsPage
        && channelTabs->currentWidget() == channelsPage;
    auto* tree = ui->channelList;
    const auto filterChannel = [&](QTreeWidgetItem* row, Backend& owner,
                                   bool directMessages) {
        const QString channelId = row->data(0, SidebarItem::IdRole).toString();
        const QString accountId = row->data(0, SidebarItem::AccountIdRole).toString();
        const auto* channel = owner.getStorage().getChannelById(channelId);
        auto& sidebar = SidebarService::instance(owner);
        bool matchesText = !textFilterActive
            || row->text(0).contains(filterText, Qt::CaseInsensitive);
        if (channel && channel->type == BackendChannel::directChannel && textFilterActive) {
            if (const auto* user = owner.getStorage().getUserById(channel->name)) {
                matchesText = matchesText
                    || user->getDisplayName().contains(filterText, Qt::CaseInsensitive)
                    || user->username.contains(filterText, Qt::CaseInsensitive)
                    || user->email.contains(filterText, Qt::CaseInsensitive);
            }
        }
        const bool retained = channelsTabVisible && channelId == retainedUnreadFilterChannelId
            && accountId == retainedUnreadFilterAccountId;
        const bool matchesUnread = !unreadGateActive || (channel
            && !sidebar.isChannelMuted(*channel)
            && (sidebar.isChannelUnread(*channel) || retained));
        const bool excluded = directMessages && textFilterActive
            && row->data(0, SidebarItem::ConversationSearchExcludedRole).toBool();
        const bool visible = matchesText && matchesUnread && !excluded;
        row->setHidden(!visible);
        return visible;
    };

    const auto filterTeam = [&](QTreeWidgetItem* team) {
        const QString accountId = team->data(0, SidebarItem::AccountIdRole).toString();
        auto* owner = tree->backendForAccount(accountId);
        if (!owner || !tree->accountVisible(accountId)) {
            team->setHidden(true);
            return false;
        }
        auto& sidebar = SidebarService::instance(*owner);
        bool hasVisibleRows = false;
        for (int i = 0; i < team->childCount(); ++i) {
            auto* row = team->child(i);
            const int kind = row->data(0, SidebarItem::KindRole).toInt();
            if (kind == SidebarItem::VirtualDestination) {
                const bool visible = SidebarUnreadPolicy::virtualDestinationVisible(
                    textFilterActive, row->text(0).contains(filterText, Qt::CaseInsensitive));
                row->setHidden(!visible);
                hasVisibleRows = hasVisibleRows || visible;
                continue;
            }
            if (kind != SidebarItem::Category) continue;
            const auto* state = sidebar.teamState(row->data(0, SidebarItem::TeamIdRole).toString());
            const auto* category = state
                ? state->category(row->data(0, SidebarItem::IdRole).toString()) : nullptr;
            const bool direct = category && category->type == QStringLiteral("direct_messages");
            const bool channels = category && category->type == QStringLiteral("channels");
            row->setData(0, SidebarItem::CategoryActionRole, direct || channels);
            row->setToolTip(0, direct ? tr("Start conversation")
                : (channels ? tr("Browse public channels") : QString()));
            bool visibleChannels = false;
            for (int j = 0; j < row->childCount(); ++j)
                visibleChannels = filterChannel(row->child(j), *owner, direct) || visibleChannels;
            row->setHidden(anyFilterActive && !visibleChannels);
            hasVisibleRows = hasVisibleRows || visibleChannels;
        }
        team->setHidden(anyFilterActive && !hasVisibleRows);
        return hasVisibleRows;
    };

    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        auto* root = tree->topLevelItem(i);
        const int kind = root->data(0, SidebarItem::KindRole).toInt();
        if (kind == SidebarItem::Team) {
            filterTeam(root);
        } else if (kind == SidebarItem::AccountRoot) {
            bool visible = false;
            for (int j = 0; j < root->childCount(); ++j)
                visible = filterTeam(root->child(j)) || visible;
            root->setHidden(anyFilterActive && !visible);
        } else if (kind == SidebarItem::SharedConversations) {
            bool visible = false;
            for (int j = 0; j < root->childCount(); ++j) {
                auto* row = root->child(j);
                auto* owner = tree->backendForAccount(
                    row->data(0, SidebarItem::AccountIdRole).toString());
                if (owner) visible = filterChannel(row, *owner, true) || visible;
                else row->setHidden(true);
            }
            root->setHidden(anyFilterActive && !visible);
        }
    }
}

void MainWindow::showCollectionPage(PostCollectionView* page)
{
    if (!page || !ui || !ui->chatAreaStackedWidget) {
        return;
    }
    if (ChatArea* current = ui->channelList->getCurrentPage()) {
        current->onDeactivate();
    }
    if (ui->chatAreaStackedWidget->indexOf(page) < 0) {
        ui->chatAreaStackedWidget->addWidget(page);
    }
    ui->chatAreaStackedWidget->setCurrentWidget(page);
}

void MainWindow::openSavedMessages(const QString& teamId)
{
    if (!backendOrNull()) return;
    Q_UNUSED(teamId)
    if (!savedMessagesPage || &savedMessagesPage->backendInstance() != backendOrNull()) {
        savedMessagesPage = nullptr;
        for (auto* page : ui->chatAreaStackedWidget->findChildren<PostCollectionView*>(
                QString(), Qt::FindDirectChildrenOnly)) {
            if (&page->backendInstance() == backendOrNull()
                && page->viewMode() == PostCollectionView::Mode::Saved) {
                savedMessagesPage = page;
                break;
            }
        }
    }
    if (!savedMessagesPage) {
        savedMessagesPage = new PostCollectionView(
            backendInstance(), PostCollectionView::Mode::Saved, ui->chatAreaStackedWidget);
    }
    showCollectionPage(savedMessagesPage);
    savedMessagesPage->activateSaved();
}

void MainWindow::openDrafts(const QString& teamId)
{
    if (!backendOrNull()) return;
    Q_UNUSED(teamId)
    if (!draftsPage || &draftsPage->backendInstance() != backendOrNull()) {
        draftsPage = nullptr;
        for (auto* page : ui->chatAreaStackedWidget->findChildren<PostCollectionView*>(
                QString(), Qt::FindDirectChildrenOnly)) {
            if (&page->backendInstance() == backendOrNull()
                && page->viewMode() == PostCollectionView::Mode::Drafts) {
                draftsPage = page;
                break;
            }
        }
    }
    if (!draftsPage) {
        draftsPage = new PostCollectionView(
            backendInstance(), PostCollectionView::Mode::Drafts,
            ui->chatAreaStackedWidget);
        auto* page = draftsPage.data();
        connect(page, &PostCollectionView::draftActivated, this,
                [this, page](const QString& channelId, const QString& rootId,
                             const QString& draftKey) {
            openDraftForBackend(page->backendInstance(), channelId, rootId, draftKey);
        });
    }
    showCollectionPage(draftsPage);
    draftsPage->activateDrafts();
}

void MainWindow::openMessageSearch()
{
    openMessageSearchInChannel(QString());
}

void MainWindow::openMessageSearchInChannel(const QString& channelId)
{
    if (!backendOrNull()) return;
    openMessageSearchForBackend(backendInstance(), channelId);
}

void MainWindow::openMessageSearchForBackend(Backend& source, const QString& channelId)
{
    if (!NavigationUiController::instance(*this).canPresentBackend(source)) return;
    if (!searchMessagesPage || &searchMessagesPage->backendInstance() != &source) {
        searchMessagesPage = nullptr;
        for (auto* page : ui->chatAreaStackedWidget->findChildren<PostCollectionView*>(
                QString(), Qt::FindDirectChildrenOnly)) {
            if (&page->backendInstance() == &source
                && page->viewMode() == PostCollectionView::Mode::Search) {
                searchMessagesPage = page;
                break;
            }
        }
    }
    if (!searchMessagesPage) {
        searchMessagesPage = new PostCollectionView(
            source, PostCollectionView::Mode::Search, ui->chatAreaStackedWidget);
    }

    const bool scopedSearch = !channelId.isEmpty();
    BackendChannel* channel = scopedSearch
        ? source.getStorage().getChannelById(channelId)
        : source.getCurrentChannel();

    QString preferredTeamId;
    QString presetTerms;
    if (channel) {
        preferredTeamId = channel->team
            ? channel->team->id : source.getCurrentTeamContextId();
        if (scopedSearch) {
            presetTerms = ConversationReference::searchTerms(*channel);
        }
    }

    // Search is a transient destination, not the selected sidebar channel. Clear
    // currentItem so clicking the previously active channel will always navigate
    // back even though its tree row did not otherwise change.
    ui->channelList->setCurrentItem(nullptr);
    showCollectionPage(searchMessagesPage);
    searchMessagesPage->activateSearch(preferredTeamId, presetTerms);
}

void MainWindow::openDirectMessageSearch()
{
    if (!backendOrNull()) return;
	UserSearchDialog::showConversationPicker(backendInstance(), this);
}

void MainWindow::createMenu()
{
    mainMenu = new QMenu(ui->toolButton);
    mainMenu->setObjectName(QStringLiteral("sidebarMainMenu"));
    connect(mainMenu, &QMenu::aboutToShow, mainMenu, [this] {
        IconUtils::refreshMenuActionIcons(*mainMenu);
    });

    QMenu* fileMenu = mainMenu->addMenu("File");
    fileMenu->setObjectName(QStringLiteral("sidebarFileMenu"));
    IconUtils::setMenuActionIcon(*fileMenu->menuAction(),
                                 QStringLiteral(":/icons/menu-folder"));
    QAction* quitAction = fileMenu->addAction(tr("Quit"), qApp, &QApplication::quit);
    quitAction->setObjectName(QStringLiteral("sidebarQuitAction"));
    IconUtils::setMenuActionIcon(*quitAction,
                                 QStringLiteral(":/icons/menu-power"));
    quitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Q")));
    quitAction->setShortcutContext(Qt::ApplicationShortcut);
    quitAction->setMenuRole(QAction::QuitRole);
    addAction(quitAction);
    QAction* logoutAction = fileMenu->addAction("Logout", this, [this] {
        // The application must destroy all chat/post widgets while their
        // Backend::Storage objects are still alive. Never reset the backend
        // from a callback that retains this MainWindow.
        if (backendOrNull()) emit logoutRequested();
    });
    logoutAction->setObjectName(QStringLiteral("sidebarLogoutAction"));
    IconUtils::setMenuActionIcon(*logoutAction,
                                 QStringLiteral(":/icons/menu-logout"));

    QAction* groupsAction = mainMenu->addAction(tr("User groups…"), [this] {
        if (!backendOrNull()) return;
        auto* dialog = new UserGroupsDialog(backendInstance(), this);
        dialog->show();
    });
    groupsAction->setObjectName(QStringLiteral("sidebarUserGroupsAction"));
    IconUtils::setMenuActionIcon(*groupsAction,
                                 QStringLiteral(":/icons/members"));

    QAction* settingsAction = mainMenu->addAction("Settings", [this] {
		auto* settingsWindow = new SettingsWindow(this);
		settingsWindow->setAttribute(Qt::WA_DeleteOnClose);
		connect(settingsWindow, &QDialog::accepted, settingsWindow,
		        [settingsWindow] { settingsWindow->applyNewSettings(); });
        settingsWindow->show();
    });
    settingsAction->setObjectName(QStringLiteral("sidebarSettingsAction"));
    IconUtils::setMenuActionIcon(*settingsAction,
                                 QStringLiteral(":/icons/menu-settings"));

    QMenu* helpMenu = mainMenu->addMenu("Help");
    helpMenu->setObjectName(QStringLiteral("sidebarHelpMenu"));
    IconUtils::setMenuActionIcon(*helpMenu->menuAction(),
                                 QStringLiteral(":/icons/menu-help"));
#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
    if (auto* updater = qApp->findChild<WindowsUpdateController*>()) {
        QAction* restart = helpMenu->addAction(tr("Update and restart"),
            updater, &WindowsUpdateController::startUpdateAndRestart);
        restart->setEnabled(updater->canApply());
        restart->setObjectName(QStringLiteral("sidebarUpdateRestartAction"));
        IconUtils::setMenuActionIcon(*restart,
                                     QStringLiteral(":/icons/refresh"));

        // Opening Help -> Check for updates is a read-only view of the
        // existing controller. The automatic check never creates or shows a
        // window and opening this dialog never starts a second network request.
        auto* statusDialog = new QDialog(this);
        statusDialog->setWindowTitle(tr("MatterLeast updates"));
        statusDialog->setAttribute(Qt::WA_QuitOnClose, false);
        statusDialog->setWindowModality(Qt::NonModal);
        statusDialog->setMinimumWidth(400);

        auto* layout = new QVBoxLayout(statusDialog);
        auto* channelLabel = new QLabel(statusDialog);
        layout->addWidget(channelLabel);
        auto* sourceLabel = new QLabel(statusDialog);
        sourceLabel->setWordWrap(true);
        sourceLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(sourceLabel);
        auto* statusLabel = new QLabel(statusDialog);
        statusLabel->setWordWrap(true);
        layout->addWidget(statusLabel);
        auto* lastCheckedLabel = new QLabel(statusDialog);
        lastCheckedLabel->setWordWrap(true);
        layout->addWidget(lastCheckedLabel);
        auto* resultLabel = new QLabel(statusDialog);
        resultLabel->setWordWrap(true);
        layout->addWidget(resultLabel);

        auto* bar = new QProgressBar(statusDialog);
        bar->setTextVisible(true);
        layout->addWidget(bar);
        auto* buttons = new QHBoxLayout();
        layout->addLayout(buttons);
        auto* checkButton = new QPushButton(tr("Check now"), statusDialog);
        buttons->addWidget(checkButton);
        auto* applyButton = new QPushButton(tr("Update and restart"), statusDialog);
        buttons->addWidget(applyButton);
        buttons->addStretch();
        auto* closeButton = new QPushButton(tr("Close"), statusDialog);
        buttons->addWidget(closeButton);
        connect(closeButton, &QPushButton::clicked, statusDialog, &QDialog::hide);
        connect(checkButton, &QPushButton::clicked, updater,
                &WindowsUpdateController::checkNow);
        connect(applyButton, &QPushButton::clicked, updater,
                &WindowsUpdateController::startUpdateAndRestart);
        statusDialog->hide();

        const auto refresh = [this, updater, restart, channelLabel, sourceLabel,
                              statusLabel, lastCheckedLabel, resultLabel,
                              bar, checkButton, applyButton] {
            const auto state = updater->state();
            const bool busy = updater->isBusy();
            const bool available = state != WindowsUpdateController::State::Unavailable;
            const QString buildId = qApp->property("matterleast-build-id").toString();
            channelLabel->setText(
                tr("Installed: %1%2\nChannel: %3")
                    .arg(QCoreApplication::applicationVersion(),
                         buildId.isEmpty() ? QString()
                            : QStringLiteral(" (%1)").arg(buildId),
                         updater->checkChannel()));
            const auto activeChannel = updater->checkChannel() == QLatin1String("nightly")
                ? Mattermost::UpdateChannel::Nightly : Mattermost::UpdateChannel::Stable;
            sourceLabel->setText(tr("Manifest: %1").arg(
                Mattermost::updateManifestUrl(activeChannel)));

            QString status;
            switch (state) {
            case WindowsUpdateController::State::Unavailable:
                status = tr("Updates are unavailable for this installation.");
                break;
            case WindowsUpdateController::State::Idle:
                status = tr("Not checking for updates.");
                break;
            case WindowsUpdateController::State::Checking:
                status = tr("Checking for updates...");
                break;
            case WindowsUpdateController::State::Downloading:
                status = tr("Downloading update %1...").arg(updater->availableVersion());
                break;
            case WindowsUpdateController::State::Preparing:
                status = tr("Verifying and preparing %1...").arg(updater->availableVersion());
                break;
            case WindowsUpdateController::State::Ready:
                status = tr("Version %1 is ready to install.").arg(updater->availableVersion());
                break;
            case WindowsUpdateController::State::Failed:
                status = tr("The previous update operation failed.");
                break;
            }
            statusLabel->setText(status);

            const QDateTime checked = updater->lastCheckTime();
            lastCheckedLabel->setText(checked.isValid()
                ? tr("Last checked: %1").arg(QLocale().toString(
                    checked.toLocalTime(), QLocale::ShortFormat))
                : tr("Last checked: never"));
            const QString result = updater->lastCheckResult();
            resultLabel->setText(result.isEmpty()
                ? tr("No previous check result.") : result);

            bar->setVisible(busy);
            if (state == WindowsUpdateController::State::Downloading
                && updater->downloadTotalBytes() > 0) {
                bar->setRange(0, 100);
                bar->setValue(int(100 * updater->downloadedBytes()
                                   / updater->downloadTotalBytes()));
                bar->setFormat(tr("%p% (%1 / %2 MB)")
                    .arg(updater->downloadedBytes() / (1024 * 1024))
                    .arg(updater->downloadTotalBytes() / (1024 * 1024)));
            } else if (busy) {
                bar->setRange(0, 0);
            }
            checkButton->setEnabled(available && !busy);
            applyButton->setVisible(updater->canApply());
            applyButton->setEnabled(updater->canApply());
            restart->setEnabled(updater->canApply());
        };

        QAction* checkUpdatesAction = helpMenu->addAction(
            tr("Check for updates"), this, [statusDialog, refresh] {
                refresh(); // Snapshot of any background work already underway.
                statusDialog->show();
                statusDialog->raise();
                statusDialog->activateWindow();
            });
        checkUpdatesAction->setObjectName(
            QStringLiteral("sidebarCheckUpdatesAction"));
        IconUtils::setMenuActionIcon(*checkUpdatesAction,
                                     QStringLiteral(":/icons/refresh"));
        connect(updater, &WindowsUpdateController::stateChanged, this,
            [refresh](WindowsUpdateController::State) { refresh(); });
        connect(updater, &WindowsUpdateController::progressChanged, this,
            [refresh](qint64, qint64) { refresh(); });
        connect(updater, &WindowsUpdateController::checkHistoryChanged, this,
            refresh);
        connect(MLOptions::instance()->optionObject(
                    QStringLiteral("updates/channel"), QStringLiteral("stable")),
                &MLOptionObject::changed, this,
                [refresh](const QVariant&) { refresh(); });
    }
#endif
    QAction* aboutMatterLeastAction = helpMenu->addAction("About MatterLeast", [this] {
		auto* msgBox = new QMessageBox(QMessageBox::Information,
		                               "About MatterLeast", infoText());
		msgBox->setIconPixmap(windowIcon().pixmap(QSize(64, 64)));
		msgBox->setTextFormat(Qt::RichText);
		msgBox->setStandardButtons(QMessageBox::Ok);
		msgBox->setDefaultButton(QMessageBox::Ok);
        msgBox->open();
    });
    aboutMatterLeastAction->setObjectName(
        QStringLiteral("sidebarAboutMatterLeastAction"));
    IconUtils::setMenuActionIcon(*aboutMatterLeastAction,
                                 QStringLiteral(":/icons/menu-info"));

    QAction* aboutQtAction = helpMenu->addAction("About QT", [this] {
        QMessageBox::aboutQt(this, "About QT");
    });
    aboutQtAction->setObjectName(QStringLiteral("sidebarAboutQtAction"));
    IconUtils::setMenuActionIcon(*aboutQtAction,
                                 QStringLiteral(":/icons/menu-info"));

    ui->toolButton->setMenu(mainMenu);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
		if (watched == ui->toolButton) {
			refreshMenuButtonIcon();
        } else if (watched == ui->searchButton) {
            refreshSearchButtonIcon();
        } else if (unreadFilterButton
                   && (watched == unreadFilterButton
                       || watched == unreadFilterButton->parentWidget())) {
            // Follow the same toolbar palette event that updates the # and +
            // buttons; the filter's own stylesheet may keep stale colors.
            QTimer::singleShot(0, this, [this] { refreshUnreadFilterIcon(); });
        }
	}
	return QMainWindow::eventFilter(watched, event);
}

void MainWindow::refreshMenuButtonIcon()
{
	if (!ui || !ui->toolButton) {
		return;
	}
	ui->toolButton->setIcon(IconUtils::tintedSymbolicIcon(
		QStringLiteral(":/icons/burger"),
		ui->toolButton->palette().color(QPalette::ButtonText)));
    if (QMenu* menu = ui->toolButton->menu()) {
        IconUtils::refreshMenuActionIcons(*menu);
    }
}

void MainWindow::refreshSearchButtonIcon()
{
    if (!ui || !ui->searchButton) {
        return;
    }
    ui->searchButton->setIcon(IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/search"),
        ui->searchButton->palette().color(QPalette::ButtonText)));
}

void MainWindow::refreshUnreadFilterIcon()
{
	if (!unreadFilterButton) {
		return;
	}

    // Compass icon-filter-variant (F0236), the same glyph as the official
    // Mattermost client. Never depend on theme icons or private-use fonts:
    // Windows has neither and used to fall back to an information icon.
    // The checked-state stylesheet can retain an outdated button palette
    // across system theme changes. The surrounding team toolbar receives the
    // correct inherited palette (also used for # and +), so use that instead.
    const QWidget* toolbar = unreadFilterButton->parentWidget();
    const QPalette palette = toolbar ? toolbar->palette()
                                     : QApplication::palette();
    // Match the explicit checked background instead of relying on native
    // styles to choose a contrasting foreground for a pre-tinted icon.
    const QColor color = unreadFilterButton->isChecked()
        ? palette.color(QPalette::HighlightedText)
        : palette.color(QPalette::ButtonText);
    unreadFilterButton->setIcon(IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/filter-variant"), color));
	unreadFilterButton->setToolTip(unreadFilterButton->isChecked()
		? tr("Show all channels")
		: tr("Show unread only"));
}

void MainWindow::moveEvent(QMoveEvent*)
{
	ChatArea* currentPage = ui->channelList->getCurrentPage();
	if (currentPage) {
		currentPage->onMove(pos());
	}
}

void MainWindow::dragMoveEvent(QDragMoveEvent*)
{
	qDebug() << "dragMove" << mapToGlobal(pos());
}

void MainWindow::changeEvent(QEvent* event)
{
	QWidget::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        // Refresh after Qt finishes propagating the new palette to child
        // buttons. IconUtils produces tinted QPixmaps, not live SVG icons.
        QTimer::singleShot(0, this, [this] {
            refreshMenuButtonIcon();
            refreshSearchButtonIcon();
            refreshUnreadFilterIcon();
        });
    }
	if (event->type() == QEvent::ActivationChange) {
		if (isActiveWindow()) {
            // A docked thread and the central chat may both have visible
            // timelines. Hidden tabs and detached windows do not belong to
            // this activation and must not be materialized or touched.
            for (ChatArea* area : findChildren<ChatArea*>()) {
                if (area && area->window() == this && area->isVisible()) {
                    area->onMainWindowActivate();
                }
            }
		}
	} else {
		qDebug() << event->type();
	}
}

void MainWindow::prepareForLogout()
{
    if (doDeinit) {
        return;
    }
    doDeinit = true;
    // closeEvent writes navigation/sidebar state while their backing storage
    // is valid. The application destroys this widget after the QAction stack
    // has unwound and only then initiates the asynchronous server logout.
    close();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	qDebug() << "closeEvent";
	if (doDeinit) {
		qDebug() << "QMainWindow closeEvent";
		saveState();
		return QMainWindow::closeEvent(event);
	}

	if (trayIcon.isVisible()) {
		hide();
		event->ignore();
	}
}

void MainWindow::initializationComplete()
{
	teamChannelsReady = true;
	restoreNavigationSessionWhenReady();
	LOG_DEBUG("MainWindow initialization comlete");
	if (!currentTeamRestoredFromSettings) {
		// No persisted team was restored. ChannelTree will retain its first
		// usable selection until the user explicitly chooses another one.
	}
}

void MainWindow::restoreNavigationSessionWhenReady()
{
    if (!backendOrNull()) return;
	if (!channelMembershipsReady || !teamChannelsReady || navigationSessionRestoreScheduled) return;
	for (const auto& team : backendInstance().getStorage().teams) {
		if (!populatedSessionSidebars.contains(team.first)) return;
	}
	// Wait for the existing sidebar requests/rendering; restoration must not
	// duplicate category HTTP requests merely to establish startup readiness.
	navigationSessionRestoreScheduled = true;
	NavigationUiController::instance(*this).restoreSession();
}

void MainWindow::unreadMessagesNotify(const BackendChannel& channel)
{
	Q_UNUSED(channel);
}

void MainWindow::setNotificationsCountVisualization(uint32_t notificationsCount)
{
    const QString productName = qApp->applicationDisplayName();
    setWindowTitle(notificationsCount == 0
        ? productName
        : QStringLiteral("(%1) %2").arg(notificationsCount).arg(productName));
}

void MainWindow::saveState()
{
	NavigationUiController::instance(*this).saveSession();
	LOG_DEBUG("MainWindow saveState");
	auto* options = MLOptions::instance();
    options->setValue(QStringLiteral("geometry"), saveGeometry());
    if (sidebarSplitter && sidebarSplitter->isVisible()) {
        const auto sizes = sidebarSplitter->sizes();
        if (sizes.size() == 2 && sizes[0] > 0 && sizes[1] > 1)
            options->setValue(QStringLiteral("sidebar_splitter_state"), sidebarSplitter->saveState());
    }
}

} /* namespace Mattermost */
