/**
 * @file ServerUiService.h
 * @brief Routes server-originated transient UI requests without coupling the backend to widgets.
 */

#pragma once

#include <QObject>
#include <QHash>

#include <functional>
#include <QJsonObject>
#include <QString>

namespace Mattermost {

class Backend;

class ServerUiService : public QObject {
    Q_OBJECT
public:
    static ServerUiService& instance(Backend& backend);

    void requestInteractiveDialog(const QJsonObject& dialog,
                                  const QString& url,
                                  const QString& channelId,
                                  const QString& teamId);
    void notifyEphemeralMessage(const QString& message);

    // Registered by each view. Multiple chat/thread views can synchronously
    // accept the same transient post without storing it in persistent history.
    void registerEphemeralPostHandler(
        QObject* owner, std::function<bool(const QJsonObject&)> handler);
    void unregisterEphemeralPostHandler(QObject* owner);
    bool dispatchEphemeralPost(const QJsonObject& post);
    void notifyCustomWebSocketEvent(const QString& eventName,
                                    const QJsonObject& data,
                                    const QJsonObject& broadcast);

signals:
    void interactiveDialogRequested(const QJsonObject& dialog,
                                    const QString& url,
                                    const QString& channelId,
                                    const QString& teamId);
    void ephemeralMessageReceived(const QString& message);
    void customWebSocketEventReceived(const QString& eventName,
                                      const QJsonObject& data,
                                      const QJsonObject& broadcast);

private:
    explicit ServerUiService(Backend& backend);
    QHash<QObject*, std::function<bool(const QJsonObject&)>> ephemeralHandlers_;
};

} // namespace Mattermost
