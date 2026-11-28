/**
 * @file WebSocketConnector.h
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 28, 2021
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include <memory>
#include <QObject>
#include <QJsonObject>

class QString;
class QUrl;

namespace Mattermost {

class WebSocketEventHandler;

class WebSocketConnector: public QObject {
	Q_OBJECT
public:
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        WaitingForReconnect,
    };
    Q_ENUM(ConnectionState)

	WebSocketConnector (WebSocketEventHandler& eventHandler);
	~WebSocketConnector () override;
public:
	void open (const QString& urlString, const QString& authToken);
	void close ();
	void reset ();
	void doHandshake ();
    ConnectionState connectionState() const;
    QString connectionId() const;
    QString serverVersion() const;
    void reconnectNow();
    void probeNow();
    // Official desktop presence request; manual is always false for activity.
    void reportUserActivity(bool active);
    bool sendUserTyping(const QString& channelId, const QString& parentId);
    bool sendCallsPluginAction(const QString& action, const QJsonObject& data);
    bool sendCallsSdp(const QJsonObject& description);
    bool typingEnabled() const;
    int typingIntervalMs() const;
    int typingMaximumMembers() const;
signals:
	void onConnect (bool isReconnect, bool needsHttpResync);
	void onDisconnect ();
    void connectionStateChanged(ConnectionState state);
    void serverVersionChanged();
private:
    void setConnectionState(ConnectionState state);
    void setServerVersion(const QString& version);
	void onNewPacket (const QString& string);
	void notifyReadyIfVerified ();
	void openSocket ();
	void startHeartbeat ();
	void stopHeartbeat ();
	void sendPing ();
	QUrl socketUrl () const;
public:
	WebSocketEventHandler& eventHandler;
private:
	struct Private;
	std::unique_ptr<Private> d;
};

} /* namespace Mattermost */
