#include "NavigationUiController.h"
#include "mainwindow.h"
#include "ViewNavigationSession.h"

#include <QApplication>
#include <algorithm>
#include <cmath>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVariant>
#include <utility>

#include "AppNavigationService.h"
#include "backend/Backend.h"
#include "backend/PostRepository.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {
using Location = NavigationUiController::Location;

QJsonObject encode(const Location& location)
{
    QJsonObject result {{"channel", location.channelId}, {"root", location.rootId}};
    if (location.viewportAnchor && location.viewportAnchor->isValid()
        && std::isfinite(static_cast<double>(location.viewportAnchor->fraction))) {
        const auto& anchor = *location.viewportAnchor;
        result.insert("anchor", QJsonObject {
            {"post", anchor.postId},
            {"bottom", anchor.bottom},
            {"fraction", anchor.fraction}
        });
    }
    return result;
}

Location decode(const QJsonObject& object, bool restoreAnchors)
{
    Location location;
    location.channelId = object.value("channel").toString();
    location.rootId = object.value("root").toString();
    // Version 1 sessions predate measured post-relative anchors. Their legacy
    // "post" field is not precise enough to restore and must be ignored.
    if (!restoreAnchors) return location;

    const QJsonObject stored = object.value("anchor").toObject();
    const QString postId = stored.value("post").toString();
    const QJsonValue offset = stored.value("fraction");
    const QJsonValue bottom = stored.value("bottom");
    if (!postId.isEmpty() && offset.isDouble() && bottom.isBool()
        && std::isfinite(offset.toDouble())) {
        location.postId = postId;
        location.viewportAnchor = NavigationTabsModel::ViewportAnchor {
            postId, bottom.toBool(), offset.toDouble()};
    }
    return location;
}

// One positioning implementation for tab, window, session and Back/Forward
// bookmarks. Post resolution and sparse thread indexing remain the source's
// responsibility; this helper only applies the saved semantic geometry.
bool restorePosition(ChatLogWidget* view, const QString& postId,
                     std::optional<NavigationTabsModel::ViewportAnchor> anchor)
{
    if (!view || postId.isEmpty()) return false;
    if (!anchor) return view->restoreViewportBookmark(postId);
    return view->restoreSemanticViewportBookmark({
        anchor->postId,
        anchor->bottom ? LongListWidget::SemanticAnchorPoint::ItemBottom
                       : LongListWidget::SemanticAnchorPoint::ItemCenter,
        anchor->fraction});
}
}

QString NavigationUiController::sessionSettingsKey() const
{
    Backend* owner = backend();
    if (!owner || !owner->getStorage().loginUser
        || owner->networkSession().host().isEmpty()) return {};
    const QString user = owner->getStorage().loginUser->id;
    if (user.isEmpty()) return {};
    const QByteArray account = (owner->networkSession().host() + QChar(0x1f) + user).toUtf8();
    return QStringLiteral("navigation_session/")
        + QString::fromLatin1(QCryptographicHash::hash(account, QCryptographicHash::Sha256).toHex());
}



ViewNavigationSession* NavigationUiController::historySession() const
{
    return viewNavigationSession ? viewNavigationSession.data() : startupHistory;
}

bool NavigationUiController::hasBoundView() const
{
    return !viewNavigationSession.isNull();
}

ViewNavigationSession::Location NavigationUiController::semanticLocation(
    const Location& source) const
{
    if (!source.valid()) return {};
    if (hasBoundView())
        return viewNavigationSession->allowsAccount(source.accountId) ? source : Location{};
    return backendForAccount(source.accountId) ? source : Location{};
}

bool NavigationUiController::presentationLocation(
    const ViewNavigationSession::Location& source, Location& result) const
{
    if (!source.valid()) return false;
    result = source;
    if (hasBoundView())
        return viewNavigationSession->allowsAccount(source.accountId);
    return backendForAccount(result.accountId) != nullptr;
}

void NavigationUiController::syncViewSession()
{
    if (shuttingDown || applyingViewSession || restoringSession || !sessionRestored
        || !viewNavigationSession) return;

    // Back/Forward already belongs to the view session; this is strictly a
    // reconciliation of visual tabs, the current bookmark and dock geometry.
    auto state = viewNavigationSession->snapshot();
    state.tabs.clear();
    state.tabs.reserve(tabModel.count());
    for (int i = 0; i < tabModel.count(); ++i) {
        auto entry = *tabModel.at(i);
        if (!viewNavigationSession->allowsAccount(entry.accountId)) continue;
        state.tabs.append(std::move(entry));
    }
    state.activeTab = -1;
    if (const auto* selected = tabModel.at(activeTabIndex)) {
        Location location = semanticLocation(tabLocation(*selected));
        for (int i = 0; i < state.tabs.size(); ++i)
            if (state.tabs[i].accountId == location.accountId
                && state.tabs[i].channelId == location.channelId
                && state.tabs[i].rootId == location.rootId) {
                state.activeTab = i;
                break;
            }
    }
    // The view session owns the semantic current destination. A deferred
    // Widgets channel activation must not roll it back during a Back/Forward
    // replay or while the target is still being admitted asynchronously.
    // Only initialize an empty view from a previously restored legacy UI.
    if (!state.current.valid() && currentLocation.valid())
        state.current = semanticLocation(currentLocation);

    auto* dock = threadStack && !threadStack->isHidden()
        ? qobject_cast<ChatArea*>(threadStack->currentWidget()) : nullptr;
    if (dock && dock->isThread) {
        state.dock = semanticLocation(captureLocation(dock));
        state.dockVisible = state.dock.valid();
    } else {
        // A previously visible dock must not reappear when this view is
        // activated again after the user explicitly closed the right pane.
        state.dock = {};
        state.dockVisible = false;
    }
    // Keep semantic detached windows of disconnected accounts. Connected
    // accounts supply the authoritative set of currently open windows.
    QVector<ViewNavigationSession::DetachedThread> retained;
    for (const auto& detached : state.detached)
        if (!backendForAccount(detached.location.accountId))
            retained.append(detached);
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (!area || !area->isThread || !area->isVisible()
            || !area->property("threadDetached").toBool()
            || area->property("navigationViewId").toString() != viewNavigationSession->viewId())
            continue;
        const Location location = semanticLocation(captureLocation(area));
        if (!location.valid() || !viewNavigationSession->allowsAccount(location.accountId))
            continue;
        retained.append({location, area->saveGeometry()});
    }
    state.detached = std::move(retained);
    if (!viewNavigationSession->restoreSnapshot(state))
        qWarning("Unable to transfer Widgets navigation state into its view session");
}

void NavigationUiController::unbindViewSession()
{
    if (!viewNavigationSession) return;
    if (sessionRestored) {
        saveActiveTabLocation();
        syncViewSession();
    }

    const QString outgoingView = viewNavigationSession->viewId();
    viewNavigationSession.clear();
    viewNavigationMembers.clear();

    // Detached threads remain owned by their original view, and cannot
    // accidentally appear above a different server's active workspace.
    const bool wasApplying = applyingViewSession;
    const bool wasSwitching = switchingTabs;
    applyingViewSession = true;
    switchingTabs = true;
    // The dock is a view-specific presentation. Its semantic bookmark has
    // already been transferred, so retire only the live ChatArea and timeline.
    closeThreadDockIfDifferent({}, {}, true);
    {
        for (QWidget* widget : QApplication::allWidgets()) {
            auto* thread = qobject_cast<ChatArea*>(widget);
            if (!thread || !thread->isThread
                || !window.admitsNavigationBackend(thread->getBackend())
                || !thread->property("threadDetached").toBool()) continue;
            const QString ownerView = thread->property("navigationViewId").toString();
            if (ownerView.isEmpty()) {
                thread->setProperty("navigationViewId", outgoingView);
                thread->hide();
            } else if (ownerView == outgoingView) {
                thread->hide();
            }
        }
    }
    switchingTabs = wasSwitching;
    applyingViewSession = wasApplying;
    updateHistoryButtons();
}

void NavigationUiController::bindViewSession(ViewNavigationSession* session)
{
    if (!session) return;
    if (viewNavigationSession == session) {
        if (viewNavigationMembers != session->allowedAccounts()) {
            viewNavigationMembers = session->allowedAccounts();
            restoreBoundViewSession();
        }
        return;
    }
    const bool hadView = viewNavigationSession != nullptr;
    const QString oldViewId = hadView ? viewNavigationSession->viewId() : QString();
    if (hadView && sessionRestored) {
        saveActiveTabLocation();
        if (activeArea && currentLocation.isChat()) {
            const Location latest = captureLocation(activeArea);
            if (latest.valid() && latest.sameDestination(currentLocation))
                currentLocation = latest;
        }
        syncViewSession();
    }

    viewNavigationSession = session;
    viewNavigationMembers = session->allowedAccounts();

    // Detached threads remain associated with their original view, rather
    // than being silently reused by a different view of the same account.
    // Suppress reentrant navigation/model updates from QWidget show/hide while
    // the outgoing presentation is still installed in the central surface.
    const bool previousApplying = applyingViewSession;
    const bool previousSwitching = switchingTabs;
    applyingViewSession = true;
    switchingTabs = true;
    {
        for (QWidget* widget : QApplication::allWidgets()) {
            auto* thread = qobject_cast<ChatArea*>(widget);
            if (!thread || !thread->isThread
                || !window.admitsNavigationBackend(thread->getBackend())
                || !thread->property("threadDetached").toBool()) continue;
            QString attachedView = thread->property("navigationViewId").toString();
            if (attachedView.isEmpty()) {
                attachedView = oldViewId.isEmpty() ? session->viewId() : oldViewId;
                thread->setProperty("navigationViewId", attachedView);
            }
            if (attachedView == session->viewId()) {
                if (!thread->isVisible()) {
                    thread->setAttribute(Qt::WA_ShowWithoutActivating, true);
                    thread->show();
                }
            } else {
                thread->hide();
            }
        }
    }
    switchingTabs = previousSwitching;
    applyingViewSession = previousApplying;
    // Hide outgoing detached threads *before* tab restoration; findThread()
    // must never adopt another view's visible detached presentation.
    // A View can be active before authentication. Its own snapshot wins
    // over any standalone account-wide restart state.
    sessionRestored = true;
    restoreBoundViewSession();
}

void NavigationUiController::restoreBoundViewSession()
{
    if (!sessionRestored || !viewNavigationSession || !navigationTabs)
        return;
    const auto state = viewNavigationSession->snapshot();
    auto sameAccount = [this](const ViewNavigationSession::Location& location) {
        return !location.valid()
            || viewNavigationSession->allowsAccount(location.accountId);
    };
    for (const auto& tab : state.tabs)
        if (!viewNavigationSession->allowsAccount(tab.accountId)) return;
    if (!sameAccount(state.current) || !sameAccount(state.dock)) return;
    for (const auto& location : state.back)
        if (!sameAccount(location)) return;
    for (const auto& location : state.forward)
        if (!sameAccount(location)) return;

    // Invalidate deferred post/channel admission from the outgoing view.
    // Old asynchronous bookmark callbacks inspect these properties before
    // applying viewport state to a reused channel ChatArea.
    window.beginSemanticNavigation();
    if (mainStack) {
        for (int i = 0; i < mainStack->count(); ++i) {
            auto* area = qobject_cast<ChatArea*>(mainStack->widget(i));
            if (!area) continue;
            area->setProperty("sessionBookmark", QString());
            area->setProperty("sessionBookmarkAnchor", QVariant());
        }
    }

    const bool oldRestoring = restoringSession;
    const bool oldSwitching = switchingTabs;
    const bool oldReplaying = replayingHistory;
    applyingViewSession = true;
    pendingHistoryThread.reset();
    pendingContextualThread.reset();
    restoringSession = true;
    switchingTabs = true;
    replayingHistory = true;

    // Materialized thread tabs belong to the old presentation only. The
    // target view retains its lightweight semantic bookmarks, never widgets.
    if (navigationSurfaceStack && mainStack) {
        navigationSurfaceStack->setCurrentWidget(mainStack);
        for (int i = navigationSurfaceStack->count() - 1; i >= 0; --i) {
            auto* thread = qobject_cast<ChatArea*>(navigationSurfaceStack->widget(i));
            if (!thread || !thread->property("threadTabbed").toBool()) continue;
            thread->onDeactivate();
            thread->hide();
            navigationSurfaceStack->removeWidget(thread);
            thread->setProperty("threadTabbed", false);
            if (activeArea == thread) activeArea = nullptr;
            thread->close();
        }
    }
    closeThreadDockIfDifferent({}, {}, true);

    {
        QSignalBlocker blocker(navigationTabs);
        activeTabIndex = -1;
        activeArea = nullptr;
        currentLocation = {};
        QVector<NavigationTabsModel::Entry> desired;
        for (const auto& entry : state.tabs) {
            Location location;
            if (!presentationLocation(tabLocation(entry), location)) continue;
            auto projected = entry;
            projected.accountId = location.accountId;
            if (backendForAccount(location.accountId))
                projected.title = tabTitle(location);
            desired.append(std::move(projected));
        }

        // Apply the destination delta, retaining tabs shared by both views.
        // The QtCore model and the QTabBar survive every View switch.
        for (int i = tabModel.count() - 1; i >= 0; --i) {
            const auto* existing = tabModel.at(i);
            const bool kept = std::any_of(desired.cbegin(), desired.cend(),
                [existing](const auto& entry) { return existing->sameDestination(entry); });
            if (!kept) {
                tabModel.remove(i);
                navigationTabs->removeTab(i);
            }
        }
        for (int i = 0; i < desired.size(); ++i) {
            const auto& entry = desired.at(i);
            const int old = tabModel.findDestination(entry.channelId, entry.rootId, entry.accountId);
            if (old < 0) {
                tabModel.insert(i, entry);
                navigationTabs->insertTab(i, entry.title);
            } else {
                if (old != i) {
                    tabModel.move(old, i);
                    navigationTabs->moveTab(old, i);
                }
                tabModel.replace(i, entry);
                tabModel.setPinned(i, entry.pinned);
                if (entry.viewportAnchor)
                    tabModel.setViewportAnchor(i, *entry.viewportAnchor);
                else
                    tabModel.clearViewportAnchor(i);
            }
            setNavigationTabTitle(i, entry.title);
            refreshTabUnreadVisual(entry.accountId, entry.channelId);
        }
    }

    int target = state.activeTab;
    if (target < 0 || target >= tabModel.count())
        target = tabModel.isEmpty() ? -1 : 0;
    if (target >= 0)
        activateTab(target);
    else
        showEmptyConversation();

    Location semanticCurrent;
    if (presentationLocation(state.current, semanticCurrent)
        && semanticCurrent.valid()
        && (!semanticCurrent.isChat()
            || !currentLocation.sameDestination(semanticCurrent)))
        navigateTo(semanticCurrent);

    if (state.dockVisible && state.dock.rootId.size()) {
        Location dock;
        if (presentationLocation(state.dock, dock)) {
            Backend* owner = backendForAccount(dock.accountId);
            BackendChannel* channel = owner
                ? owner->getStorage().getChannelById(dock.channelId) : nullptr;
            if (channel) {
                ChatArea* area = findThreadForBackend(*owner, dock.channelId, dock.rootId);
                if (!area)
                    area = new ChatArea(*owner, *channel, dock.rootId, nullptr);
                attachThread(area);
                if (!dock.postId.isEmpty())
                    restoreSessionBookmark(area, dock.postId, dock.viewportAnchor);
            }
        }
    }

    // Restore only windows belonging to an attached account in this View.
    // Unavailable accounts retain lightweight descriptors for later admission.
    for (const auto& detached : state.detached) {
        Location location;
        if (!presentationLocation(detached.location, location)) continue;
        Backend* owner = backendForAccount(location.accountId);
        BackendChannel* channel = owner
            ? owner->getStorage().getChannelById(location.channelId) : nullptr;
        if (!channel) continue;
        ChatArea* area = findThreadForBackend(*owner, location.channelId, location.rootId);
        if (!area) {
            area = new ChatArea(*owner, *channel, location.rootId, nullptr);
            ensureThreadButton(area);
        }
        area->setProperty("threadDetached", true);
        area->setProperty("navigationViewId", viewNavigationSession->viewId());
        if (!detached.geometry.isEmpty()) area->restoreGeometry(detached.geometry);
        updateThreadButton(area);
        area->setAttribute(Qt::WA_ShowWithoutActivating, true);
        area->show();
        if (!location.postId.isEmpty())
            restoreSessionBookmark(area, location.postId, location.viewportAnchor);
    }

    if (presentationLocation(state.current, semanticCurrent))
        currentLocation = semanticCurrent;

    replayingHistory = oldReplaying;
    switchingTabs = oldSwitching;
    restoringSession = oldRestoring;
    applyingViewSession = false;
    refreshTabBarVisibility();
    if (state.current.valid()
        && state.current.kind != ViewNavigationSession::Location::Kind::Chat)
        navigationTabs->hide();
    updateHistoryButtons(); // Synchronizes the settled semantic snapshot.
}

void NavigationUiController::scheduleSessionSave()
{
    if (!shuttingDown && sessionRestored && !restoringSession && !applyingViewSession) {
        syncViewSession();
        if (sessionSaveTimer) sessionSaveTimer->start();
    }
}

void NavigationUiController::trackSessionArea(ChatArea* area)
{
    if (!area || area->property("sessionTracked").toBool()) return;
    area->setProperty("sessionTracked", true);
    // The area can be destroyed during the MainWindow QWidget teardown.
    // beginShutdown() captures the final snapshot before that starts.
    connect(area, &QObject::destroyed, this, [this] { scheduleSessionSave(); });
    connect(area, &ChatArea::aboutToDeactivate, this, [this, area] {
        if (area->isThread || restoringSession || switchingTabs) return;
        const auto* entry = tabModel.at(activeTabIndex);
        if (entry && entry->rootId.isEmpty()
            && entry->accountId == accountIdForBackend(area->getBackend())
            && entry->channelId == area->getChannel().id) {
            // ChannelTree switches its central stack only after deactivation.
            // Snapshot the old viewport now, while the source is attached.
            saveActiveTabLocation();
        }
    });
    if (auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"))) {
        // Snapshot semantics on explicit session saves (not every pixel
        // movement), avoiding disk-write storms during ordinary scrolling.
        connect(log, &LongListWidget::userScrollStarted, area, [area] {
            area->setProperty("sessionBookmark", QString());
            area->setProperty("sessionBookmarkAnchor", QVariant());
        });
        connect(log, &LongListWidget::materializedRangeChanged, area,
                [area, log](int, int) {
            const QString pending = area->property("sessionBookmark").toString();
            if (pending.isEmpty()) return;
            // A source can receive a target body before it knows the target's
            // authoritative row. Retry using the *same* post positioning path
            // when the presentation acquires more concrete geometry.
            QTimer::singleShot(0, area, [area, log, pending] {
                if (area->property("sessionBookmark").toString() != pending) return;
                const QVariantMap data =
                    area->property("sessionBookmarkAnchor").toMap();
                std::optional<NavigationTabsModel::ViewportAnchor> anchor;
                if (data.value(QStringLiteral("post")).toString() == pending) {
                    anchor = NavigationTabsModel::ViewportAnchor {
                        pending,
                        data.value(QStringLiteral("bottom")).toBool(),
                        data.value(QStringLiteral("fraction")).toDouble()};
                }
                if (restorePosition(log, pending, anchor)
                    && log->findPost(pending)) {
                    area->setProperty("sessionBookmark", QString());
                    area->setProperty("sessionBookmarkAnchor", QVariant());
                }
            });
        });
    }
}

void NavigationUiController::restoreSessionBookmark(
    ChatArea* area, const QString& postId,
    std::optional<NavigationTabsModel::ViewportAnchor> anchor)
{
    if (!area || postId.isEmpty()) return;
    trackSessionArea(area);
    area->preparePostNavigation();
    auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
    if (!log) return;
    area->setProperty("sessionBookmark", postId);
    area->setProperty("sessionBookmarkAnchor", anchor
        ? QVariant(QVariantMap {
            {QStringLiteral("post"), anchor->postId},
            {QStringLiteral("bottom"), anchor->bottom},
            {QStringLiteral("fraction"), anchor->fraction}})
        : QVariant());
    auto position = [anchor, postId](ChatLogWidget* view) {
        return restorePosition(view, postId, anchor);
    };
    if (position(log)) {
        // A provisional thread slot can accept the target long before its
        // PostWidget exists. Keep the old anchor authoritative during that
        // interval, including a quick switch to another tab.
        if (log->findPost(postId)) {
            area->setProperty("sessionBookmark", QString());
            area->setProperty("sessionBookmarkAnchor", QVariant());
        }
        return;
    }

    // ChannelPostSource deliberately does not invent an absolute index for a
    // cached post. It must first adopt a bounded *server* context through the
    // same PostRepository and ChannelPostSource path as explicit post navigation.
    // Fetching only the post body (as for a thread's provisional slot) leaves
    // an ordinary channel bookmark unmapped and its viewport at newest.
    QPointer<ChatArea> guard(area);
    if (!area->isThread) {
        PostRepository::instance(area->getBackend()).loadChannelAround(
            area->getChannel(), postId,
            [guard, postId, position](const PostRepository::Context& context) {
                if (!guard || guard->property("sessionBookmark").toString() != postId)
                    return;
                if (!context.success) return; // Retain the bookmark for retry.

                guard->ensurePinnedPostVisible(
                    postId, context.postIds,
                    context.reachedOldest, context.reachedNewest);
                auto* view = guard->findChild<ChatLogWidget*>(
                    QStringLiteral("listWidget"));
                if (position(view) && view->findPost(postId)) {
                    guard->setProperty("sessionBookmark", QString());
                    guard->setProperty("sessionBookmarkAnchor", QVariant());
                }
                if (view) view->refreshReadState();
            },
            true);
        return;
    }

    // ThreadPostSource already supports provisional indices and asynchronous
    // context reconciliation. Continue through its shared positioning path.
    // User navigation/scrolling cancels the pending bookmark.
    auto loadBookmark = [guard, postId, position] {
        if (!guard || guard->property("sessionBookmark").toString() != postId) return;
        PostRepository::instance(guard->getBackend()).loadPost(postId,
            [guard, postId, position](const PostRepository::PostResult& result) {
                if (!guard || guard->property("sessionBookmark").toString() != postId) return;
                auto* view = guard->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
                const bool positioned = result.success && position(view);
                if (!result.success) {
                    // A genuinely missing post cannot be restored. A
                    // successful fetch with a temporarily unavailable row is
                    // *not* evidence that the semantic destination is invalid.
                    guard->setProperty("sessionBookmark", QString());
                    guard->setProperty("sessionBookmarkAnchor", QVariant());
                    guard->goToNewest();
                } else if (positioned && view && view->findPost(postId)) {
                    guard->setProperty("sessionBookmark", QString());
                    guard->setProperty("sessionBookmarkAnchor", QVariant());
                }
                if (view) view->refreshReadState();
            });
    };
    if (area->isThread && !area->getChannel().postIdToPost.contains(area->root_id)) {
        PostRepository::instance(area->getBackend()).loadPost(area->root_id,
            [loadBookmark](const PostRepository::PostResult&) { loadBookmark(); });
    } else {
        loadBookmark();
    }
}

void NavigationUiController::saveSession()
{
    if (shuttingDown || !sessionRestored || restoringSession) return;
    // Never encode a multi-account presentation using the legacy single
    // account snapshot format (which deliberately has no account IDs).
    if (hasBoundView() || attachedBackends.size() > 1) {
        syncViewSession();
        return;
    }
    const QString key = sessionSettingsKey();
    if (key.isEmpty()) return;

    auto locationOf = [this](ChatArea* area) {
        return area ? captureLocation(area) : Location{};
    };
    saveActiveTabLocation();
    QJsonArray tabs;
    for (int i = 0; i < tabModel.count(); ++i) {
        const auto* modelEntry = tabModel.at(i);
        Location location = tabLocation(*modelEntry);
        // The model already owns the last measured bookmark for inactive
        // tabs. Re-reading a live view here would also overwrite an anchor
        // while its asynchronous restoration is still in progress.
        QJsonObject tab = encode(location);
        tab.insert("pinned", modelEntry->pinned);
        tabs.append(tab);
    }
    QJsonArray threads;
    Backend* owner = backend();
    // A right-hand thread's QStackedWidget retains older children after
    // switching. Those are historical cached views, not open dock sessions.
    ChatArea* visibleDock = threadStack && !threadStack->isHidden()
        ? qobject_cast<ChatArea*>(threadStack->currentWidget()) : nullptr;
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (!area || !area->isThread || &area->getBackend() != owner
            || area->property("threadTabbed").toBool()) continue;
        const bool detached = area->property("threadDetached").toBool();
        // Detached windows are independent visible surfaces; for the right
        // pane, persist only the currently selected and visible dock.
        if (!detached && area != visibleDock) continue;
        if (detached && !area->isVisible()) continue;
        QJsonObject entry = encode(locationOf(area));
        entry.insert("mode", detached ? "window" : "dock");
        if (detached) entry.insert("geometry", QString::fromLatin1(area->saveGeometry().toBase64()));
        threads.append(entry);
    }
    Location active;
    if (const auto* entry = tabModel.at(activeTabIndex)) active = tabLocation(*entry);
    auto* dock = visibleDock;
    auto* focused = qobject_cast<ChatArea*>(QApplication::activeWindow());
    QJsonObject state {{"version", 2}, {"tabs", tabs}, {"threads", threads},
                       {"active_tab", encode(active)},
                       {"active_dock", encode(locationOf(dock))},
                       {"dock_visible", visibleDock != nullptr},
                       {"focused_window", focused && focused->property("threadDetached").toBool()
                            ? encode(locationOf(focused)) : QJsonObject()}};
    MLOptions::instance()->setValue(key, QJsonDocument(state).toJson(QJsonDocument::Compact));
    syncViewSession();
}

void NavigationUiController::restoreSession()
{
    if (sessionRestored || !backend() || !navigationTabs) return;
    if (viewNavigationSession) {
        sessionRestored = true;
        restoreBoundViewSession();
        return;
    }
    const QString key = sessionSettingsKey();
    if (key.isEmpty()) return;
    const QJsonObject state = QJsonDocument::fromJson(
        MLOptions::instance()->value<QByteArray>(key)).object();
    sessionRestored = true;
    const int version = state.value("version").toInt();
    if (version != 1 && version != 2) return;
    const bool restoreAnchors = version >= 2;
    // Legacy disk snapshots are already scoped to this account's settings key.
    const auto decodeLocal = [this, restoreAnchors](const QJsonObject& object) {
        auto location = decode(object, restoreAnchors);
        location.accountId = presentationAccountId();
        return location;
    };
    restoringSession = true;
    const bool wasSwitching = switchingTabs;
    switchingTabs = true;
    auto valid = [this](const Location& location) {
        return location.valid() && backend()->getStorage().getChannelById(location.channelId);
    };
    {
        QSignalBlocker blocker(navigationTabs);
        while (navigationTabs->count()) navigationTabs->removeTab(0);
        tabModel = NavigationTabsModel();
        activeTabIndex = -1;
        for (const QJsonValue& value : state.value("tabs").toArray()) {
            const QJsonObject tab = value.toObject();
            const Location location = decodeLocal(tab);
            if (!valid(location)) continue;
            const int index = appendNavigationTab(location);
            tabModel.setPinned(index, tab.value("pinned").toBool());
            // Inactive tab entries remain semantic metadata only. Creating
            // ChatArea here eagerly materialized every hidden thread's posts.
            // activateTab() below constructs just the selected destination.
        }
    }
    const Location dock = decodeLocal(state.value("active_dock").toObject());
    const bool dockVisible = state.value("dock_visible").toBool();
    for (const QJsonValue& value : state.value("threads").toArray()) {
        const QJsonObject object = value.toObject();
        const Location location = decodeLocal(object);
        const QString mode = object.value("mode").toString();
        if (!valid(location) || location.rootId.isEmpty()
            || (mode != "window" && mode != "dock")
            || findThread(location.channelId, location.rootId)) continue;
        // Older session snapshots may contain hundreds of stale cached dock
        // views. Only the active visible dock represents a user-open surface.
        if (mode == "dock" &&
            (!dockVisible || !location.sameDestination(dock))) continue;
        auto* channel = backend()->getStorage().getChannelById(location.channelId);
        auto* area = new ChatArea(*backend(), *channel, location.rootId, nullptr);
        ensureThreadButton(area);
        if (mode == "window") {
            area->setProperty("threadDetached", true);
            if (viewNavigationSession)
                area->setProperty("navigationViewId", viewNavigationSession->viewId());
            area->restoreGeometry(QByteArray::fromBase64(object.value("geometry").toString().toLatin1()));
            updateThreadButton(area);
            area->show();
        } else {
            attachThread(area);
        }
        if (location.viewportAnchor) {
            restoreSessionBookmark(area, location.postId, location.viewportAnchor);
        }
    }
    const Location active = decodeLocal(state.value("active_tab").toObject());
    int index = tabModel.findDestination(active.channelId, active.rootId, active.accountId);
    if (index < 0 && !tabModel.isEmpty()) index = 0;
    if (index >= 0) {
        // Only the selected view is constructed; all other tab anchors stay
        // as lightweight metadata until the user activates those tabs.
        activateTab(index, true);
    }
    if (auto* area = findThread(dock.channelId, dock.rootId);
        area && threadStack->indexOf(area) >= 0) threadStack->setCurrentWidget(area);
    threadStack->setVisible(state.value("dock_visible").toBool() && threadStack->count() > 0);
    switchingTabs = wasSwitching;
    restoringSession = false;
    refreshTabBarVisibility();
    syncSplitterEdgeGutters();
    updateHistoryButtons();
    const Location focused = decodeLocal(state.value("focused_window").toObject());
    if (auto* area = findThread(focused.channelId, focused.rootId);
        area && area->property("threadDetached").toBool()) {
        area->raise();
        area->activateWindow();
    }
    scheduleSessionSave();
}
} // namespace Mattermost
