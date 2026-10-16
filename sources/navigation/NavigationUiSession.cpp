#include "NavigationUiController.h"

#include <QApplication>
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

#include "AppNavigationService.h"
#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
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
    Location location {object.value("channel").toString(),
                       object.value("root").toString()};
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
    if (!owner || !owner->getStorage().loginUser || NetworkRequest::host().isEmpty()) return {};
    const QString user = owner->getStorage().loginUser->id;
    if (user.isEmpty()) return {};
    const QByteArray account = (NetworkRequest::host() + QChar(0x1f) + user).toUtf8();
    return QStringLiteral("navigation_session/")
        + QString::fromLatin1(QCryptographicHash::hash(account, QCryptographicHash::Sha256).toHex());
}

void NavigationUiController::scheduleSessionSave()
{
    if (sessionRestored && !restoringSession && sessionSaveTimer) sessionSaveTimer->start();
}

void NavigationUiController::trackSessionArea(ChatArea* area)
{
    if (!area || area->property("sessionTracked").toBool()) return;
    area->setProperty("sessionTracked", true);
    connect(area, &QObject::destroyed, this, [this] { scheduleSessionSave(); });
    connect(area, &ChatArea::aboutToDeactivate, this, [this, area] {
        if (area->isThread || restoringSession || switchingTabs) return;
        const auto* entry = tabModel.at(activeTabIndex);
        if (entry && entry->rootId.isEmpty()
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
    if (area->isThread) {
        qInfo() << "TAB_RESTORE_DIAG" << "id" << postId
                << "fraction" << (anchor ? anchor->fraction : 999.0);
    }
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
    if (!sessionRestored || restoringSession) return;
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
}

void NavigationUiController::restoreSession()
{
    if (sessionRestored || !backend() || !navigationTabs) return;
    const QString key = sessionSettingsKey();
    if (key.isEmpty()) return;
    const QJsonObject state = QJsonDocument::fromJson(
        MLOptions::instance()->value<QByteArray>(key)).object();
    sessionRestored = true;
    const int version = state.value("version").toInt();
    if (version != 1 && version != 2) return;
    const bool restoreAnchors = version >= 2;
    restoringSession = true;
    const bool wasSwitching = switchingTabs;
    switchingTabs = true;
    auto valid = [this](const Location& location) {
        return location.isValid() && backend()->getStorage().getChannelById(location.channelId);
    };
    {
        QSignalBlocker blocker(navigationTabs);
        while (navigationTabs->count()) navigationTabs->removeTab(0);
        tabModel = NavigationTabsModel();
        activeTabIndex = -1;
        for (const QJsonValue& value : state.value("tabs").toArray()) {
            const QJsonObject tab = value.toObject();
            const Location location = decode(tab, restoreAnchors);
            if (!valid(location)) continue;
            const int index = appendNavigationTab(location);
            tabModel.setPinned(index, tab.value("pinned").toBool());
            // Inactive tab entries remain semantic metadata only. Creating
            // ChatArea here eagerly materialized every hidden thread's posts.
            // activateTab() below constructs just the selected destination.
        }
    }
    const Location dock = decode(state.value("active_dock").toObject(), restoreAnchors);
    const bool dockVisible = state.value("dock_visible").toBool();
    for (const QJsonValue& value : state.value("threads").toArray()) {
        const QJsonObject object = value.toObject();
        const Location location = decode(object, restoreAnchors);
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
    const Location active = decode(state.value("active_tab").toObject(), restoreAnchors);
    int index = tabModel.findDestination(active.channelId, active.rootId);
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
    backStack.clear();
    forwardStack.clear();
    refreshTabBarVisibility();
    syncSplitterEdgeGutters();
    updateHistoryButtons();
    const Location focused = decode(state.value("focused_window").toObject(), restoreAnchors);
    if (auto* area = findThread(focused.channelId, focused.rootId);
        area && area->property("threadDetached").toBool()) {
        area->raise();
        area->activateWindow();
    }
    scheduleSessionSave();
}
} // namespace Mattermost
