/**
 * @file ServerUiService.cpp
 * @brief Routes server-originated transient UI requests without coupling the backend to widgets.
 */

#include "ServerUiService.h"

#include "Backend.h"

#include <QPointer>
#include <QVector>

#include <algorithm>
#include <utility>

namespace Mattermost {

ServerUiService& ServerUiService::instance(Backend& backend)
{
    auto* service = backend.findChild<ServerUiService*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!service) {
        service = new ServerUiService(backend);
    }
    return *service;
}

ServerUiService::ServerUiService(Backend& backend)
    : QObject(&backend)
{
}

void ServerUiService::requestInteractiveDialog(const QJsonObject& dialog,
                                               const QString& url,
                                               const QString& channelId,
                                               const QString& teamId)
{
    emit interactiveDialogRequested(dialog, url, channelId, teamId);
}

void ServerUiService::pruneEphemeralPostHandlers()
{
    for (int index = ephemeralHandlers_.size() - 1; index >= 0; --index) {
        if (!ephemeralHandlers_.at(index).owner) {
            ephemeralHandlers_.removeAt(index);
        }
    }
}

void ServerUiService::registerEphemeralPostHandler(
    QObject* owner, std::function<bool(const QJsonObject&)> handler)
{
    if (!owner || !handler) {
        return;
    }
    pruneEphemeralPostHandlers();
    for (auto& entry : ephemeralHandlers_) {
        if (entry.owner == owner) {
            entry.callback = std::move(handler);
            return;
        }
    }

    // The owner is weakly referenced from the moment of registration, not
    // reconstructed from an unguarded QObject* at dispatch time.
    ephemeralHandlers_.push_back({QPointer<QObject>(owner), std::move(handler)});
    connect(owner, &QObject::destroyed, this, [this] {
        pruneEphemeralPostHandlers();
    });
}

void ServerUiService::unregisterEphemeralPostHandler(QObject* owner)
{
    for (int index = ephemeralHandlers_.size() - 1; index >= 0; --index) {
        const auto& entry = ephemeralHandlers_.at(index);
        if (!entry.owner || entry.owner == owner) {
            ephemeralHandlers_.removeAt(index);
        }
    }
}

bool ServerUiService::dispatchEphemeralPost(const QJsonObject& post)
{
    pruneEphemeralPostHandlers();
    bool handled = false;

    // Each snapshot entry already contains a lifetime-aware owner. A handler
    // may destroy or unregister another view while this loop executes.
    const auto snapshot = ephemeralHandlers_;
    for (const auto& entry : snapshot) {
        if (!entry.owner) {
            continue;
        }
        // An unregistered owner must not receive a later callback from a
        // snapshot taken before the unregistration.
        const bool stillRegistered = std::any_of(
            ephemeralHandlers_.cbegin(), ephemeralHandlers_.cend(),
            [&entry](const EphemeralHandler& live) {
                return live.owner && live.owner == entry.owner;
            });
        if (!stillRegistered) {
            continue;
        }
        // Invoke a copy: the callback can remove its own registration.
        const auto handler = entry.callback;
        if (handler(post)) {
            handled = true;
        }
    }
    return handled;
}

void ServerUiService::notifyEphemeralMessage(const QString& message)
{
    if (!message.isEmpty()) {
        emit ephemeralMessageReceived(message);
    }
}

void ServerUiService::notifyCustomWebSocketEvent(const QString& eventName,
                                                  const QJsonObject& data,
                                                  const QJsonObject& broadcast)
{
    if (!eventName.isEmpty()) {
        emit customWebSocketEventReceived(eventName, data, broadcast);
    }
}

} // namespace Mattermost
