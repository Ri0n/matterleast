/**
 * @file ServerUiService.cpp
 * @brief Routes server-originated transient UI requests without coupling the backend to widgets.
 */

#include "ServerUiService.h"

#include "Backend.h"

#include <QPointer>
#include <QVector>

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

void ServerUiService::registerEphemeralPostHandler(
    QObject* owner, std::function<bool(const QJsonObject&)> handler)
{
    if (!owner || !handler) {
        return;
    }
    if (!ephemeralHandlers_.contains(owner)) {
        connect(owner, &QObject::destroyed, this, [this, owner] {
            ephemeralHandlers_.remove(owner);
        });
    }
    ephemeralHandlers_.insert(owner, std::move(handler));
}

void ServerUiService::unregisterEphemeralPostHandler(QObject* owner)
{
    ephemeralHandlers_.remove(owner);
}

bool ServerUiService::dispatchEphemeralPost(const QJsonObject& post)
{
    bool handled = false;

    // Snapshot guarded owners: one view may destroy another while handling.
    // Callback storage remains in the registration table.
    QVector<QPointer<QObject>> owners;
    owners.reserve(ephemeralHandlers_.size());
    for (auto it = ephemeralHandlers_.cbegin(); it != ephemeralHandlers_.cend(); ++it) {
        owners.push_back(QPointer<QObject>(it.key()));
    }
    for (const auto& owner : owners) {
        if (!owner) {
            continue;
        }
        const auto it = ephemeralHandlers_.constFind(owner.data());
        if (it == ephemeralHandlers_.cend()) {
            continue;
        }
        // A callback may unregister itself or destroy the owning widget.
        // Never invoke it directly out of QHash storage that could be erased.
        const auto handler = it.value();
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
