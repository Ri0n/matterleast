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

#pragma once

#include <cstdint>
#include <memory>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QCheckBox;
class QEvent;
class QLineEdit;
class QSplitter;
class QStackedLayout;
class QSystemTrayIcon;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Mattermost {

class AttentionList;
class ChatArea;
class Backend;
class BackendChannel;
class BackendPost;
class BackendTeam;
class ChannelQuickList;
class ChannelTree;
class NotificationManager;
class PostCollectionView;
class SidebarUpdateBanner;
class ViewNavigationSession;
class AccountManager;
class ServersSidebarPage;

class MainWindow: public QMainWindow {
	Q_OBJECT
public:
	MainWindow(QWidget* parent, QSystemTrayIcon& trayIcon);
    MainWindow(QWidget* parent, QSystemTrayIcon& trayIcon, Backend& backend);
	~MainWindow();
public:
    Backend* backendOrNull() const;
    Backend& backendInstance() const;
    // Transitional attachment: build the permanent Widgets surface before
    // any network session exists. An already attached window is not rebound.
    bool attachInitialBackend(Backend& source);
    // Attach an already initialized account's navigation to the same central
    // surface. Does not switch the selected profile or restart its services.
    bool attachNavigationBackend(Backend& source);
    Backend* navigationBackend(const QString& accountId) const;
    bool admitsNavigationBackend(const Backend& source) const;
    void beginSemanticNavigationFrom(Backend* source);
    // Header identity follows the selected account, not an open chat.
    void setProfileBackend(Backend* source);
	void initializationComplete ();
    void installRealtimeUiSync();
    void bindViewNavigation(ViewNavigationSession* session, const QString& accountId = {});
    void unbindViewNavigation();
    // Shared account/profile model rendered as an icon-only tab before Channels.
    void installServersSidebar(AccountManager& manager);
    // Reapply the saved left sidebar width after navigation reparents the
    // central widget (the splitter's initial restore happens while hidden).
    void restoreSidebarWidth();
    void showServersSidebar();
    void showChannelsSidebar();
    void openAccountDestination(int destination, const QString& teamId);
    // Replace only the tree inside the existing Channels tab. The ThinSplitter
    // and all other MainWindow presentation remain unchanged.
    void setViewChannelTree(ChannelTree* tree, QWidget* parkingParent);
    ChannelTree* viewChannelTree() const;
    // Save and close while post widgets can still access valid Storage.
    void prepareForLogout();
    void beginSemanticNavigation();
	void openChannelPost(const QString& channelId,
	                     const QString& postId = QString(),
	                     const QString& rootId = QString(),
	                     const QStringList& contextPostIds = QStringList(),
	                     bool reachedOldest = false,
	                     bool reachedNewest = false,
                         bool preserveIfOpen = false);
    void openChannelPostForBackend(Backend& source, const QString& channelId,
                                   const QString& postId, const QString& rootId,
                                   const QStringList& contextPostIds,
                                   bool reachedOldest, bool reachedNewest,
                                   bool preserveIfOpen);
    void openMessageSearchInChannel(const QString& channelId);

    void showEvent(QShowEvent* event) override;
	void changeEvent (QEvent* event) override;
	void closeEvent(QCloseEvent *event) override;
	void saveState ();
	void unreadMessagesNotify (const BackendChannel& channel);
	void setNotificationsCountVisualization (uint32_t notificationsCount);
	void moveEvent (QMoveEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;

signals:
    void logoutRequested();
    void serverOpenRequested(const QString& accountId);
    void viewOpenRequested(const QString& viewId);
    void addServerRequested();
    void serverSettingsRequested(const QString& accountId);
    void serverDisconnectRequested(const QString& accountId);
    void serverConnectRequested(const QString& accountId);

private slots:
    void on_usericon_label_clicked();
    void on_channelList_virtualDestinationRequested(int destination,
                                                     const QString& teamId);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
private:
	void createMenu ();
    void reportStatusResult(bool success, const QString& reason);
    void refreshProfileHeader();
    void refreshQueueAccounts();
	void setupChannelTabs ();
	void restoreNavigationSessionWhenReady();
	void refreshSidebarViews ();
	void refreshChannelUnreadFilter ();
	void applySidebarTextFilter(QTreeWidget* tree) const;
	void openDirectMessageSearch ();
    void openMessageSearch();
    void openSavedMessages(const QString& teamId = QString());
    void openDrafts(const QString& teamId = QString());
    void openDraft(const QString& channelId, const QString& rootId,
                   const QString& draftKey = QString());
    void showCollectionPage(PostCollectionView* page);
	void refreshMenuButtonIcon ();
    void refreshSearchButtonIcon();
	void refreshUnreadFilterIcon ();
private:
	std::unique_ptr<Ui::MainWindow>		ui;
	QSystemTrayIcon&					trayIcon;
	NotificationManager*                    notificationManager;
    QPointer<Backend> _backend;
    QPointer<Backend> _profileBackend;
    QObject* _profileSubscriptions = nullptr;
    QFont _defaultProfileStatusFont;
	QSplitter*							sidebarSplitter = nullptr;
	QTabWidget*							channelTabs = nullptr;
	QWidget*							channelsPage = nullptr;
    QStackedLayout*                   channelTreePages = nullptr;
    QPointer<ChannelTree>             _viewChannelTree;
    QMetaObject::Connection           _viewTreeSearchConnection;
    ServersSidebarPage*                  serversPage = nullptr;
    QWidget*                            sidebarFilterRow = nullptr;
    QWidget*                            _unreadPage = nullptr;
    QCheckBox*                          _unreadFollowingCheck = nullptr;
    QCheckBox*                          _unreadChannelsCheck = nullptr;
    QCheckBox*                          _unreadMutedCheck = nullptr;
	QLineEdit*							sidebarFilterEdit = nullptr;
#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
    SidebarUpdateBanner*                _updateBanner = nullptr;
    bool                                _updateOfferVisible = false;
    bool                                _updateRestartRequested = false;
#endif
	QToolButton*						unreadFilterButton = nullptr;
	ChannelQuickList*					recentChannels = nullptr;
	AttentionList*						attentionList = nullptr;
    PostCollectionView*                 savedMessagesPage = nullptr;
    PostCollectionView*                 draftsPage = nullptr;
    PostCollectionView*                 recentMentionsPage = nullptr;
    PostCollectionView*                 searchMessagesPage = nullptr;
	QString								retainedUnreadFilterChannelId;
    quint64                             semanticNavigationGeneration = 0;
	bool								currentTeamRestoredFromSettings;
	bool channelMembershipsReady = false;
	bool teamChannelsReady = false;
	QSet<QString> populatedSessionSidebars;
	bool navigationSessionRestoreScheduled = false;
	QMenu*								mainMenu;
	bool								doDeinit;
};

} /* namespace Mattermost */
