/**
 * @file WebSocketConnector.cpp
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

#include "WebSocketConnector.h"

#include "HTTPConnector.h"

#include <iostream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
#include <QNetworkInformation>
#endif
#include <QRandomGenerator>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtWebSockets/QWebSocket>

#include "backend/WebSocketEventHandler.h"
#include "log.h"

namespace Mattermost {

namespace {

constexpr int HeartbeatIntervalMs = 30000;
constexpr int HeartbeatReplyTimeoutMs = 10000;
constexpr int ConnectionAttemptTimeoutMs = 15000;
constexpr int MinReconnectDelayMs = 1000;
constexpr int MaxReconnectDelayMs = 15000;
constexpr int ReconnectJitterMs = 500;
constexpr char BrowserUserAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64; rv:149.0) Gecko/20100101 Firefox/149.0";

void setWebSocketScheme(QUrl& url)
{
    if (url.scheme() == QLatin1String("https")) {
        url.setScheme(QStringLiteral("wss"));
    } else if (url.scheme() == QLatin1String("http")) {
        url.setScheme(QStringLiteral("ws"));
    }
}

QUrl websocketEndpoint(const QUrl& apiBaseUrl, const QJsonObject& config)
{
    QUrl base;
    const QString configuredUrl = config.value(QStringLiteral("WebsocketURL")).toString();
    if (!configuredUrl.isEmpty()) {
        base = QUrl(configuredUrl);
        setWebSocketScheme(base);
    }

    if (!base.isValid() || base.host().isEmpty()) {
        base = apiBaseUrl;

        QString path = base.path();
        const QString apiSuffix = QStringLiteral("api/v4/");
        if (path.endsWith(apiSuffix)) {
            path.chop(apiSuffix.size());
        }
        base.setPath(path);
        setWebSocketScheme(base);

        if (base.port() < 0) {
            const QString portKey = base.scheme() == QLatin1String("wss")
                ? QStringLiteral("WebsocketSecurePort")
                : QStringLiteral("WebsocketPort");
            bool ok = false;
            const int configuredPort = config.value(portKey).toString().toInt(&ok);
            if (ok && configuredPort > 0 && configuredPort <= 65535) {
                base.setPort(configuredPort);
            }
        }
    }

    QString path = base.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    path += QStringLiteral("/api/v4/websocket");
    base.setPath(path);
    base.setQuery(QString());
    base.setFragment(QString());
    return base;
}

QByteArray siteOrigin(const QUrl& apiBaseUrl)
{
    QUrl origin(apiBaseUrl);
    if (origin.scheme() == QLatin1String("wss")) {
        origin.setScheme(QStringLiteral("https"));
    } else if (origin.scheme() == QLatin1String("ws")) {
        origin.setScheme(QStringLiteral("http"));
    }
    origin.setPath(QString());
    origin.setQuery(QString());
    origin.setFragment(QString());
    return origin.toEncoded();
}

template<typename T>
void handler (WebSocketConnector& conn, const QJsonObject& data, const QJsonObject& broadcast)
{
	conn.eventHandler.handleEvent (T (data, broadcast));
}

const QMap<QString, void(*)(WebSocketConnector&, const QJsonObject&, const QJsonObject&)> eventHandlers {
	{"hello", [] (WebSocketConnector&, const QJsonObject&, const QJsonObject&) {
		std::cout << "Hello" << std::endl;
	}},
	{"channel_viewed",		handler<ChannelViewedEvent>},
	{"posted", 				handler<PostEvent>},
	{"post_edited", 		handler<PostEditedEvent>},
	{"post_deleted",		handler<PostDeletedEvent>},
	{"reaction_added",		handler<PostReactionAddedEvent>},
	{"reaction_removed",	handler<PostReactionRemovedEvent>},
	{"typing",				handler<TypingEvent>},
	{"status_change", 		handler<StatusChangeEvent>},
	{"direct_added", 		handler<NewDirectChannelEvent>},
	{"new_user",			handler<NewUserEvent>},
	{"user_updated",		handler<UserUpdatedEvent>},
	{"user_added",			handler<UserAddedToChannelEvent>},
	{"added_to_team",		handler<UserAddedToTeamEvent>},
	{"leave_team",			handler<UserLeaveTeamEvent>},
	{"user_removed",		handler<UserRemovedFromChannelEvent>},
	{"channel_created",		handler<ChannelCreatedEvent>},
	{"channel_updated",		handler<ChannelUpdatedEvent>},
	{"open_dialog",			handler<OpenDialogEvent>},
	{"ephemeral_message", [] (WebSocketConnector& conn, const QJsonObject& data, const QJsonObject&) {
		conn.eventHandler.handleEphemeralMessage(data);
	}},
    {"preference_changed",  handler<PreferenceChangedEvent>},
    {"preferences_changed", handler<PreferencesChangedEvent>},
    {"preferences_deleted", handler<PreferencesDeletedEvent>},
};

bool printEvent (const QString& name)
{
	if (	name == "channel_viewed" 	||
			name == "channel_updated" 	||
			name == "reaction_added" 	||
			name == "status_change" 	||
			name == "posted" 			||
			name == "reaction_removed"	||
			name == "user_removed"		||
			name == "user_updated"		||
			name == "leave_team"      ||
			name == "ephemeral_message" ||
            name == "preference_changed" ||
            name == "preferences_changed" ||
            name == "preferences_deleted"
	) {
		return false;
	}

	return true;
}

} // namespace

struct WebSocketConnector::Private {
	QWebSocket webSocket;
	QNetworkAccessManager configNetworkManager;
	QString token;
	QUrl apiBaseUrl;
	QUrl endpointUrl;
	QTimer heartbeatTimer;
	QTimer heartbeatReplyTimer;
	QTimer reconnectTimer;
	QTimer connectionAttemptTimer;
	QString connectionId;
	quint64 configGeneration = 0;
	int responseSequence = 1;
	int serverSequence = 0;
	int pendingPingSequence = 0;
	int pendingAuthenticationSequence = 0;
	int reconnectAttempt = 0;
	bool waitingForPong = false;
	bool hasReconnect = false;
	bool resumeFailed = false;
	bool helloReceived = false;
	bool connectNotified = false;
	bool immediateReconnectPending = false;
	bool suppressReconnect = false;
    ConnectionState connectionState = ConnectionState::Disconnected;
};

WebSocketConnector::WebSocketConnector (WebSocketEventHandler& eventHandler)
:eventHandler (eventHandler)
,d (std::make_unique<Private>())
{
	auto errorHandler = [this] (QAbstractSocket::SocketError error) {
		LOG_DEBUG ("WebSocket error " << error << " " << d->webSocket.errorString());
		if (!d->suppressReconnect && d->webSocket.state() == QAbstractSocket::UnconnectedState) {
			scheduleReconnect ();
		}
	};
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
	connect (&d->webSocket, &QWebSocket::errorOccurred, this, errorHandler);
#else
	connect (&d->webSocket, qOverload<QAbstractSocket::SocketError>(&QWebSocket::error), this, errorHandler);
#endif

	connect (&d->webSocket, &QWebSocket::connected, this, [this] {
		LOG_DEBUG ("WebSocket connected");
		d->reconnectTimer.stop ();
		d->responseSequence = 1;
		d->helloReceived = false;
		d->connectNotified = false;
		d->resumeFailed = false;
		doHandshake ();
		startHeartbeat ();
	});

	connect (&d->webSocket, &QWebSocket::disconnected, this, [this] {
		stopHeartbeat ();
		d->connectionAttemptTimer.stop();
		d->responseSequence = 1;

		const bool reconnectSuppressed = d->suppressReconnect;
		d->suppressReconnect = false;

		LOG_DEBUG ("WebSocket disconnected. Code: " << d->webSocket.closeCode() << " " << d->webSocket.closeReason());
		emit onDisconnect ();
		d->helloReceived = false;
		d->connectNotified = false;

		if (d->immediateReconnectPending && !reconnectSuppressed && !d->token.isEmpty()) {
			d->immediateReconnectPending = false;
			LOG_DEBUG("WebSocket restarting immediately after forced reconnect");
			openSocket();
			return;
		}
		d->immediateReconnectPending = false;

		if (!reconnectSuppressed && !d->token.isEmpty()) {
			scheduleReconnect ();
        } else {
            setConnectionState(ConnectionState::Disconnected);
		}
	});

	connect (&d->webSocket, &QWebSocket::textMessageReceived,
			 this, &WebSocketConnector::onNewPacket);

	d->heartbeatTimer.setInterval (HeartbeatIntervalMs);
	connect (&d->heartbeatTimer, &QTimer::timeout, this, &WebSocketConnector::sendPing);

	d->heartbeatReplyTimer.setSingleShot(true);
	connect(&d->heartbeatReplyTimer, &QTimer::timeout, this, [this] {
		if (!d->waitingForPong) {
			return;
		}

		LOG_DEBUG("Mattermost WebSocket ping received no response within "
		          << HeartbeatReplyTimeoutMs << " ms. Reconnecting");
		d->waitingForPong = false;
		d->pendingPingSequence = 0;
		if (d->webSocket.state() == QAbstractSocket::ConnectedState) {
			d->webSocket.abort();
		}
	});

	d->reconnectTimer.setSingleShot (true);
	connect (&d->reconnectTimer, &QTimer::timeout, this, [this] {
		if (d->token.isEmpty() || d->suppressReconnect) {
			return;
		}
		if (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
			return;
		}

		LOG_DEBUG ("WebSocket Reconnecting (connection_id=" << d->connectionId
				   << ", sequence_number=" << d->serverSequence << ")");
		openSocket ();
	});

	d->connectionAttemptTimer.setSingleShot(true);
	connect(&d->connectionAttemptTimer, &QTimer::timeout, this, [this] {
		if (d->token.isEmpty() || d->suppressReconnect
		    || d->connectionState == ConnectionState::Connected) {
			return;
		}

		LOG_DEBUG("WebSocket connection attempt timed out after "
		          << ConnectionAttemptTimeoutMs << " ms");
		if (d->webSocket.state() == QAbstractSocket::UnconnectedState) {
			scheduleReconnect();
		} else {
			d->webSocket.abort();
		}
	});

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    if (QNetworkInformation::instance() || QNetworkInformation::loadDefaultBackend()) {
        if (QNetworkInformation* networkInformation = QNetworkInformation::instance()) {
            auto networkChanged = [this] {
                if (d->connectionState == ConnectionState::Disconnected) {
                    return;
                }

                // REST and realtime use independent transports. A WebSocket can
                // survive a VPN/route change while QNetworkAccessManager is
                // still bound to dead connections, so rotate HTTP immediately.
                HTTPConnector::restartAllTransports();

                if (d->connectionState == ConnectionState::WaitingForReconnect
                    || d->connectionState == ConnectionState::Connecting) {
                    LOG_DEBUG("Network state changed while reconnecting");
                    reconnectNow();
                    return;
                }

                if (d->connectionState == ConnectionState::Connected
                    && d->webSocket.state() == QAbstractSocket::ConnectedState) {
                    LOG_DEBUG("Network state changed while connected; probing WebSocket immediately");
                    // Any outstanding ping belongs to the route that just changed.
                    // Replace it with a fresh probe and ignore a late reply to the old one.
                    d->heartbeatReplyTimer.stop();
                    d->waitingForPong = false;
                    d->pendingPingSequence = 0;
                    sendPing();
                }
            };
            connect(networkInformation, &QNetworkInformation::reachabilityChanged,
                    this, networkChanged);
            connect(networkInformation, &QNetworkInformation::transportMediumChanged,
                    this, networkChanged);
        }
    }
#endif
}

WebSocketConnector::~WebSocketConnector () = default;

WebSocketConnector::ConnectionState WebSocketConnector::connectionState() const
{
    return d->connectionState;
}

void WebSocketConnector::setConnectionState(ConnectionState state)
{
    if (d->connectionState == state) {
        return;
    }
    LOG_DEBUG("WebSocket connection state " << static_cast<int>(d->connectionState)
              << " -> " << static_cast<int>(state));
    d->connectionState = state;
    emit connectionStateChanged(state);
}

QString WebSocketConnector::connectionId() const
{
    return d->connectionId;
}

void WebSocketConnector::reconnectNow()
{
    if (d->token.isEmpty() || d->suppressReconnect
        || d->connectionState == ConnectionState::Connected) {
        return;
    }

    d->reconnectTimer.stop();
    d->connectionAttemptTimer.stop();
    LOG_DEBUG("WebSocket reconnect requested immediately (connection_state="
              << static_cast<int>(d->connectionState)
              << ", socket_state=" << static_cast<int>(d->webSocket.state())
              << ", connection_id=" << d->connectionId
              << ", sequence_number=" << d->serverSequence << ")");

    if (d->webSocket.state() == QAbstractSocket::UnconnectedState) {
        d->immediateReconnectPending = false;
        openSocket();
        return;
    }

    d->immediateReconnectPending = true;
    d->webSocket.abort();
    QTimer::singleShot(0, this, [this] {
        if (!d->immediateReconnectPending || d->token.isEmpty()
            || d->suppressReconnect
            || d->webSocket.state() != QAbstractSocket::UnconnectedState) {
            return;
        }
        d->immediateReconnectPending = false;
        openSocket();
    });
}

void WebSocketConnector::open (const QString& urlString, const QString& authToken)
{
	d->apiBaseUrl = QUrl(urlString);
	d->token = authToken;
	d->connectionId.clear ();
	d->responseSequence = 1;
	d->serverSequence = 0;
	d->pendingPingSequence = 0;
	d->pendingAuthenticationSequence = 0;
	d->reconnectAttempt = 0;
	d->waitingForPong = false;
	d->hasReconnect = false;
	d->resumeFailed = false;
	d->helloReceived = false;
	d->connectNotified = false;
	d->immediateReconnectPending = false;
	d->suppressReconnect = false;
	d->reconnectTimer.stop ();
	d->connectionAttemptTimer.stop();
	stopHeartbeat ();
    d->endpointUrl.clear();
    setConnectionState(ConnectionState::Connecting);

	const quint64 generation = ++d->configGeneration;
	QUrl configUrl = d->apiBaseUrl;
	QString configPath = configUrl.path();
	if (!configPath.endsWith(QLatin1Char('/'))) {
		configPath += QLatin1Char('/');
	}
	configPath += QStringLiteral("config/client");
	configUrl.setPath(configPath);
	configUrl.setQuery(QStringLiteral("format=old"));

	QNetworkRequest request(configUrl);
	request.setRawHeader("User-Agent", BrowserUserAgent);
	request.setRawHeader("X-Requested-With", "XMLHttpRequest");
	if (!d->token.isEmpty()) {
		request.setRawHeader("Cookie", "MMAUTHTOKEN=" + d->token.toUtf8());
	}

	QNetworkReply* reply = d->configNetworkManager.get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
		if (generation != d->configGeneration || d->token.isEmpty()) {
			reply->deleteLater();
			return;
		}

		QJsonObject config;
		if (reply->error() == QNetworkReply::NoError) {
			const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
			if (document.isObject()) {
				config = document.object();
			}
		} else {
			LOG_DEBUG("Mattermost client config request failed: "
			          << reply->error() << " " << reply->errorString()
			          << ". Falling back to the login URL for WebSocket");
		}
		reply->deleteLater();

		d->endpointUrl = websocketEndpoint(d->apiBaseUrl, config);
		LOG_DEBUG("Mattermost WebSocket endpoint resolved to "
		          << d->endpointUrl.toString(QUrl::RemovePassword));
		openSocket();
	});
}

void WebSocketConnector::close ()
{
	d->token.clear ();
	reset ();
}

void WebSocketConnector::scheduleReconnect ()
{
	stopHeartbeat ();

	if (d->token.isEmpty() || d->suppressReconnect || d->immediateReconnectPending
	    || d->reconnectTimer.isActive()) {
		return;
	}
	if (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
		return;
	}

	d->hasReconnect = true;
	++d->reconnectAttempt;

	const int exponent = qBound(0, d->reconnectAttempt - 1, 4);
	int delay = qMin(MaxReconnectDelayMs, MinReconnectDelayMs * (1 << exponent));
	if (delay < MaxReconnectDelayMs) {
		delay = qMin(MaxReconnectDelayMs,
		             delay + static_cast<int>(QRandomGenerator::global()->bounded(
		                         static_cast<quint32>(ReconnectJitterMs))));
	}

	LOG_DEBUG ("WebSocket reconnect attempt " << d->reconnectAttempt
		       << " scheduled in " << delay << " ms");
    setConnectionState(ConnectionState::WaitingForReconnect);
	d->reconnectTimer.start (delay);
}

QUrl WebSocketConnector::socketUrl () const
{
	QUrl url (d->endpointUrl);
	QUrlQuery query (url);
	query.removeAllQueryItems (QStringLiteral("connection_id"));
	query.removeAllQueryItems (QStringLiteral("sequence_number"));
	query.removeAllQueryItems (QStringLiteral("posted_ack"));
	query.addQueryItem (QStringLiteral("connection_id"), d->connectionId);
	query.addQueryItem (QStringLiteral("sequence_number"), QString::number(d->serverSequence));
	query.addQueryItem (QStringLiteral("posted_ack"), QStringLiteral("true"));
	url.setQuery (query);
	return url;
}

void WebSocketConnector::openSocket ()
{
	if (d->token.isEmpty() || d->endpointUrl.isEmpty()) {
		return;
	}

    setConnectionState(ConnectionState::Connecting);
	const QUrl url = socketUrl ();
	LOG_DEBUG ("WebSocket opening " << url.toString(QUrl::RemovePassword));

	QNetworkRequest request(url);
	request.setRawHeader("User-Agent", BrowserUserAgent);
	request.setRawHeader("Origin", siteOrigin(d->apiBaseUrl));
	request.setRawHeader("Cookie", "MMAUTHTOKEN=" + d->token.toUtf8());
	request.setRawHeader("Pragma", "no-cache");
	request.setRawHeader("Cache-Control", "no-cache");
	d->connectionAttemptTimer.start(ConnectionAttemptTimeoutMs);
	d->webSocket.open (request);
}

void WebSocketConnector::doHandshake ()
{
	QJsonObject jsonData {
		{"token", d->token},
	};

	const int sequence = d->responseSequence++;
	d->pendingAuthenticationSequence = sequence;
	QJsonDocument json (QJsonObject {
		{"seq", sequence},
		{"action", "authentication_challenge"},
		{"data", jsonData},
	});

	d->webSocket.sendTextMessage (json.toJson(QJsonDocument::Compact));
}

void WebSocketConnector::startHeartbeat ()
{
	stopHeartbeat ();
	sendPing ();
	d->heartbeatTimer.start ();
}

void WebSocketConnector::stopHeartbeat ()
{
	d->heartbeatTimer.stop ();
	d->heartbeatReplyTimer.stop();
	d->waitingForPong = false;
	d->pendingPingSequence = 0;
}

void WebSocketConnector::sendPing ()
{
	if (d->webSocket.state() != QAbstractSocket::ConnectedState) {
		return;
	}

	if (d->waitingForPong) {
		return;
	}

	const int sequence = d->responseSequence++;
	d->pendingPingSequence = sequence;
	d->waitingForPong = true;
	d->heartbeatReplyTimer.start(HeartbeatReplyTimeoutMs);

	QJsonDocument json (QJsonObject {
		{"seq", sequence},
		{"action", "ping"},
	});
	d->webSocket.sendTextMessage (json.toJson(QJsonDocument::Compact));
}

void WebSocketConnector::reset ()
{
	++d->configGeneration;
	d->reconnectTimer.stop ();
	d->connectionAttemptTimer.stop();
	stopHeartbeat ();
	d->connectionId.clear ();
	d->responseSequence = 1;
	d->serverSequence = 0;
	d->pendingAuthenticationSequence = 0;
	d->reconnectAttempt = 0;
	d->hasReconnect = false;
	d->resumeFailed = false;
	d->helloReceived = false;
	d->connectNotified = false;
	d->immediateReconnectPending = false;
    setConnectionState(ConnectionState::Disconnected);

	if (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
		d->suppressReconnect = true;
		d->webSocket.close (QWebSocketProtocol::CloseCodeNormal, QStringLiteral("Client Close"));
	} else {
		d->suppressReconnect = false;
	}
}

void WebSocketConnector::onNewPacket (const QString& string)
{
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson (string.toUtf8(), &parseError);
	if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
		LOG_DEBUG ("Invalid WebSocket JSON: " << parseError.errorString());
		return;
	}

	const QJsonObject jsonObject = doc.object ();

	const QJsonValue seqReply = jsonObject.value (QStringLiteral("seq_reply"));
	if (!seqReply.isUndefined()) {
		const int replySequence = seqReply.toInt ();
		if (d->waitingForPong && replySequence == d->pendingPingSequence) {
			d->heartbeatReplyTimer.stop();
			d->waitingForPong = false;
			d->pendingPingSequence = 0;
		}

		const bool actionFailed = jsonObject.contains(QStringLiteral("error"));
		if (replySequence == d->pendingAuthenticationSequence) {
			d->pendingAuthenticationSequence = 0;
			if (actionFailed) {
				LOG_DEBUG("WebSocket authentication challenge failed: "
				          << doc.toJson(QJsonDocument::Compact));
				d->webSocket.abort();
				return;
			}

			if (!d->connectNotified) {
				d->connectNotified = true;
				d->connectionAttemptTimer.stop();
				d->reconnectAttempt = 0;
				const bool isReconnect = d->hasReconnect;
				setConnectionState(ConnectionState::Connected);
				emit onConnect(isReconnect, false);
			}
		}

		if (actionFailed) {
			LOG_DEBUG ("WebSocket action failed: " << doc.toJson(QJsonDocument::Compact));
		}
		return;
	}

	const QString eventName = jsonObject.value (QStringLiteral("event")).toString ();

	if (eventName == QStringLiteral("hello")) {
		d->helloReceived = true;
		const QString oldConnectionId = d->connectionId;
		const QString newConnectionId = jsonObject.value(QStringLiteral("data"))
			.toObject().value(QStringLiteral("connection_id")).toString();

		if (d->hasReconnect) {
			// A resumed Mattermost stream keeps the same connection_id. Only a
			// different (or missing) id means the backlog could not be replayed
			// and the Backend really needs its expensive HTTP fallback sync.
			d->resumeFailed = oldConnectionId.isEmpty() || newConnectionId.isEmpty()
				|| oldConnectionId != newConnectionId;
		}

		if (!oldConnectionId.isEmpty() && !newConnectionId.isEmpty()
			&& oldConnectionId != newConnectionId) {
			LOG_DEBUG ("Mattermost started a new WebSocket stream (old connection_id="
					   << oldConnectionId << ", new connection_id=" << newConnectionId
					   << "). Falling back to HTTP resync");
			d->serverSequence = 0;
		} else if (d->hasReconnect && !d->resumeFailed) {
			LOG_DEBUG ("Mattermost WebSocket stream resumed successfully; skipping HTTP resync");
		}

		if (!newConnectionId.isEmpty()) {
			d->connectionId = newConnectionId;
		}
	}

	const QJsonValue eventSequenceValue = jsonObject.value (QStringLiteral("seq"));
	if (!eventSequenceValue.isUndefined()) {
		const int eventSequence = eventSequenceValue.toInt (-1);
		if (eventSequence != d->serverSequence) {
			LOG_DEBUG ("Missed WebSocket event: received seq=" << eventSequence
					   << ", expected seq=" << d->serverSequence
					   << ". Reconnecting with reliable sequence recovery");
			d->webSocket.abort ();
			return;
		}
		d->serverSequence = eventSequence + 1;
	}

	const bool sequencedEvent = !eventSequenceValue.isUndefined();
	if (!d->connectNotified
	    && (eventName == QStringLiteral("hello") || sequencedEvent)) {
		d->connectNotified = true;
		d->connectionAttemptTimer.stop();
		d->reconnectAttempt = 0;

		const bool isReconnect = d->hasReconnect;
		const bool needsHttpResync = isReconnect && d->resumeFailed;
		if (isReconnect && eventName != QStringLiteral("hello")) {
			LOG_DEBUG("Mattermost resumed the WebSocket stream without a hello event; "
			          "a correctly sequenced event proves the resumed stream is live");
		}
		d->hasReconnect = false;
		d->resumeFailed = false;
        setConnectionState(ConnectionState::Connected);
		emit onConnect(isReconnect, needsHttpResync);
	} else if (d->connectNotified && d->hasReconnect
	           && eventName == QStringLiteral("hello")) {
		const bool needsHttpResync = d->resumeFailed;
		d->hasReconnect = false;
		d->resumeFailed = false;
		if (needsHttpResync) {
			LOG_DEBUG("Reliable WebSocket resume was rejected after authentication");
			emit reliableResumeFailed();
		}
	} else if (d->connectNotified && d->hasReconnect && sequencedEvent) {
		LOG_DEBUG("Mattermost reliable WebSocket resume confirmed by sequenced event");
		d->hasReconnect = false;
		d->resumeFailed = false;
	}

	auto it = eventHandlers.find (eventName);
	if (it == eventHandlers.end()) {
        if (eventName.startsWith(QStringLiteral("custom_"))) {
            eventHandler.handleCustomEvent(
                eventName,
                jsonObject.value(QStringLiteral("data")).toObject(),
                jsonObject.value(QStringLiteral("broadcast")).toObject());
            return;
        }

		LOG_DEBUG ("Unhandled WebSocket event '" << eventName << "'\n");
		const QString jsonString = doc.toJson (QJsonDocument::Indented);
		std::cout << jsonString.toStdString ();
		qDebug() << "========" << '\n';
		return;
	}

	if (printEvent (it.key())) {
		qDebug() << "========" << '\n';
		const QString jsonString = doc.toJson (QJsonDocument::Indented);
		std::cout << jsonString.toStdString ();
	}

	it.value() (*this,
				jsonObject.value (QStringLiteral("data")).toObject(),
				jsonObject.value (QStringLiteral("broadcast")).toObject());
}

} /* namespace Mattermost */