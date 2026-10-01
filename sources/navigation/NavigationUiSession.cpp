#include "NavigationUiController.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>

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
    return {{"channel", location.channelId}, {"root", location.rootId}, {"post", location.postId}};
}

Location decode(const QJsonObject& object)
{
    return {object.value("channel").toString(), object.value("root").toString(),
            object.value("post").toString()};
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
    if (auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"))) {
        connect(log, &LongListWidget::visibleRangeChanged, this,
                [this](int, int) { scheduleSessionSave(); });
        connect(log, &LongListWidget::userScrollStarted, area, [area] {
            area->setProperty("sessionBookmark", QString());
        });
    }
}

void NavigationUiController::restoreSessionBookmark(ChatArea* area, const QString& postId)
{
    if (!area || postId.isEmpty()) return;
    trackSessionArea(area);
    area->preparePostNavigation();
    auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
    if (!log) return;
    area->setProperty("sessionBookmark", postId);
    if (log->restoreViewportBookmark(postId)) {
        area->setProperty("sessionBookmark", QString());
        return;
    }

    // Keep the requested identity in subsequent snapshots while cold bodies
    // arrive. User navigation/scrolling cancels the asynchronous positioning.
    QPointer<ChatArea> guard(area);
    auto loadBookmark = [guard, postId] {
        if (!guard || guard->property("sessionBookmark").toString() != postId) return;
        PostRepository::instance(guard->getBackend()).loadPost(postId,
            [guard, postId](const PostRepository::PostResult& result) {
                if (!guard || guard->property("sessionBookmark").toString() != postId) return;
                auto* view = guard->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
                const bool positioned = result.success && view && view->restoreViewportBookmark(postId);
                guard->setProperty("sessionBookmark", QString());
                if (!positioned) guard->goToNewest();
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
        Location location = captureLocation(area);
        auto* log = area ? area->findChild<ChatLogWidget*>(QStringLiteral("listWidget")) : nullptr;
        if (log && log->source() && log->isAtEnd()
            && area->property("sessionBookmark").toString().isEmpty())
            location.postId.clear();
        return location;
    };
    saveActiveTabLocation();
    QJsonArray tabs;
    for (int i = 0; i < tabModel.count(); ++i) {
        const auto* modelEntry = tabModel.at(i);
        Location location = tabLocation(*modelEntry);
        ChatArea* area = location.rootId.isEmpty() ? nullptr
            : findThread(location.channelId, location.rootId);
        auto* central = channelTree ? channelTree->getCurrentPage() : nullptr;
        if (location.rootId.isEmpty() && central && central->getChannel().id == location.channelId)
            area = central;
        if (area) location = locationOf(area);
        QJsonObject tab = encode(location);
        tab.insert("pinned", modelEntry->pinned);
        tabs.append(tab);
    }
    QJsonArray threads;
    Backend* owner = backend();
    for (QWidget* widget : QApplication::allWidgets()) {
        auto* area = qobject_cast<ChatArea*>(widget);
        if (!area || !area->isThread || &area->getBackend() != owner
            || area->property("threadTabbed").toBool()) continue;
        const bool detached = area->property("threadDetached").toBool();
        if (!detached && (!threadStack || threadStack->indexOf(area) < 0)) continue;
        QJsonObject entry = encode(locationOf(area));
        entry.insert("mode", detached ? "window" : "dock");
        if (detached) entry.insert("geometry", QString::fromLatin1(area->saveGeometry().toBase64()));
        threads.append(entry);
    }
    Location active;
    if (const auto* entry = tabModel.at(activeTabIndex)) active = tabLocation(*entry);
    auto* central = channelTree ? channelTree->getCurrentPage() : nullptr;
    auto* dock = threadStack ? qobject_cast<ChatArea*>(threadStack->currentWidget()) : nullptr;
    auto* focused = qobject_cast<ChatArea*>(QApplication::activeWindow());
    QJsonObject state {{"version", 1}, {"tabs", tabs}, {"threads", threads},
                       {"active_tab", encode(active)}, {"central", encode(locationOf(central))},
                       {"active_dock", encode(locationOf(dock))},
                       {"dock_visible", threadStack && !threadStack->isHidden()},
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
    if (state.value("version").toInt() != 1) return;
    restoringSession = true;
    const bool wasSwitching = switchingTabs;
    switchingTabs = true;
    auto valid = [this](const Location& location) {
        return location.isValid() && backend()->getStorage().getChannelById(location.channelId);
    };
    const Location central = decode(state.value("central").toObject());
    if (valid(central)) navigateTo(central);
    {
        QSignalBlocker blocker(navigationTabs);
        while (navigationTabs->count()) navigationTabs->removeTab(0);
        tabModel = NavigationTabsModel();
        activeTabIndex = -1;
        for (const QJsonValue& value : state.value("tabs").toArray()) {
            const QJsonObject tab = value.toObject();
            const Location location = decode(tab);
            if (!valid(location)) continue;
            const int index = appendNavigationTab(location);
            tabModel.setPinned(index, tab.value("pinned").toBool());
            if (!location.rootId.isEmpty()) {
                activateTab(index, false);
                if (auto* area = findThread(location.channelId, location.rootId))
                    restoreSessionBookmark(area, location.postId);
            }
        }
    }
    for (const QJsonValue& value : state.value("threads").toArray()) {
        const QJsonObject object = value.toObject();
        const Location location = decode(object);
        const QString mode = object.value("mode").toString();
        if (!valid(location) || location.rootId.isEmpty()
            || (mode != "window" && mode != "dock")
            || findThread(location.channelId, location.rootId)) continue;
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
        restoreSessionBookmark(area, location.postId);
    }
    const Location active = decode(state.value("active_tab").toObject());
    int index = tabModel.findDestination(active.channelId, active.rootId);
    if (index < 0 && !tabModel.isEmpty()) index = 0;
    if (index >= 0) {
        const Location location = tabLocation(*tabModel.at(index));
        activateTab(index, location.rootId.isEmpty());
    } else if (valid(central)) {
        navigateTo({central.channelId, {}, {}});
        restoreSessionBookmark(channelTree->getCurrentPage(), central.postId);
    }
    const Location dock = decode(state.value("active_dock").toObject());
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
    const Location focused = decode(state.value("focused_window").toObject());
    if (auto* area = findThread(focused.channelId, focused.rootId);
        area && area->property("threadDetached").toBool()) {
        area->raise();
        area->activateWindow();
    }
    scheduleSessionSave();
}
} // namespace Mattermost
