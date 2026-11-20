/**
 * @file Backend.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 11, 2021
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
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#include "Backend.h"
#include "backend/ChannelUnreadPostSelection.h"
#include "LoginResponsePolicy.h"

#include <iostream>
#include <memory>
#include <utility>
#include <limits>
#include <QSharedPointer>
#include <QtWebSockets/QWebSocket>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "QByteArrayCreator.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QUuid>
#include <QUrl>
#include <QDebug>
#include <QList>

#include "NetworkRequest.h"
#include "ServerUiService.h"
#include "CustomEmojiService.h"
#include "UploadTrace.h"
#include "AvatarImage.h"
#include "PublicChannelPaging.h"
#include "RealtimeFallbackService.h"
#include "types/BackendPoll.h"
#include "types/BackendNewPollData.h"
#include "plugins/PluginManager.h"
#include "reactions/ReactionUsageTracker.h"
#include "log.h"

namespace Mattermost {

namespace RequestTrackerID {
enum type {
	channelMember,
	teamMember,
};
}

bool Backend::sendCallsSdp(const QJsonObject& description)
{
    return loggedIn() && webSocketConnector.sendCallsSdp(description);
}

bool Backend::sendCallsPluginAction(const QString& action, const QJsonObject& data)
{
    return loggedIn() && webSocketConnector.sendCallsPluginAction(action, data);
}

void Backend::requestCallsPlugin(const QString& method,
                                 const QString& relativePath,
                                 const QJsonObject& body,
                                 QObject* context,
                                 CallsRequestCompletion completion)
{
    if (!context || !completion) {
        return;
    }
    const QPointer<QObject> lifetime(context);
    // An HTTP request may outlive the native plugin. Merely checking a
    // QPointer is insufficient: destroying a std::function whose target was
    // compiled inside an unloaded .so can still execute unloaded code.
    // Clear the callback synchronously as QObject is destroyed, while
    // QPluginLoader still has the provider library resident.
    const auto pending = std::make_shared<CallsRequestCompletion>(
        std::move(completion));
    QObject::connect(context, &QObject::destroyed, this,
                     [pending] { *pending = {}; });
    auto reject = [lifetime, pending](const QString& reason) {
        if (lifetime && *pending) {
            (*pending)(0, QJsonObject{{QStringLiteral("error"), reason}});
        }
    };

    // Only official Calls-plugin paths are permitted. URL, host, headers,
    // token, and cookies are constructed by the main client's network layer.
    if (!loggedIn()) {
        reject(QStringLiteral("Mattermost session is not authenticated"));
        return;
    }
    if (!relativePath.startsWith(QStringLiteral("plugins/com.mattermost.calls/"))
        || relativePath.contains(QStringLiteral(".."))
        || relativePath.contains(QLatin1Char('\\'))
        || relativePath.contains(QLatin1Char('#'))
        || relativePath.contains(QStringLiteral("://"))) {
        reject(QStringLiteral("Unsupported Calls API path"));
        return;
    }
    if (method != QLatin1String("GET") && method != QLatin1String("POST")
        && method != QLatin1String("PUT")) {
        reject(QStringLiteral("Unsupported Calls API method"));
        return;
    }

    // Calls is a server plugin served at /plugins/..., not /api/v4/.
    // Retain the backend authentication while selecting the correct route.
    NetworkRequest request(_networkSession, QString(), relativePath);
    const HttpResponseCallback handler(
        [lifetime, pending, method](QVariant, QByteArray payload,
                                    const QNetworkReply& reply) {
            if (!lifetime || !*pending) {
                return; // Callbacks were cleared before plugin unload.
            }
            const int status = reply.attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status < 200 || status >= 300) {
                // Log the effective URL after redirects, never credentials,
                // query parameters, cookies or response bodies.
                const QString url = reply.url().toString(
                    QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
                qWarning().noquote()
                    << "MatterLeast Calls HTTP request failed:"
                    << method << url
                    << "status=" << status
                    << "networkError=" << static_cast<int>(reply.error())
                    << "contentType=" << reply.header(
                           QNetworkRequest::ContentTypeHeader).toString();
            }
            (*pending)(status, QJsonDocument::fromJson(payload).object());
        });
    if (method == QLatin1String("GET")) {
        httpConnector.get(request, handler);
    } else if (method == QLatin1String("POST")) {
        httpConnector.post(request, QJsonDocument(body), handler);
    } else {
        httpConnector.put(request, QJsonDocument(body), handler);
    }
}

bool Backend::handleEphemeralMessage(const QJsonObject& data)
{
    // Mattermost encodes websocket "post" as a JSON string. Decode at the
    // backend boundary rather than the event/transport adapter.
    const QJsonValue postValue = data.value(QStringLiteral("post"));
    QJsonObject post;
    if (postValue.isString()) {
        post = QJsonDocument::fromJson(postValue.toString().toUtf8()).object();
    } else if (postValue.isObject()) {
        post = postValue.toObject();
    }
    if (post.isEmpty()) {
        return false;
    }

    auto& ui = ServerUiService::instance(*this);
    const bool handled = ui.dispatchEphemeralPost(post);
    if (!handled) {
        ui.notifyEphemeralMessage(post.value(QStringLiteral("message")).toString());
    }
    return handled;
}

bool Backend::hasVisibleTypingView(const QString& channelId,
                                   const QString& parentId) const
{
    return !_typingViews.value(TypingActivityState::contextKey(
        channelId, parentId)).isEmpty();
}

void Backend::dropTypingView(QObject* view)
{
    for (auto it = _typingViews.begin(); it != _typingViews.end();) {
        it->remove(view);
        if (it->isEmpty()) {
            const QString context = it.key();
            it = _typingViews.erase(it);
            _lastTypingSentMs.remove(context);
            if (_typingActivity.clearContext(
                    TypingActivityState::channelFromKey(context),
                    TypingActivityState::parentFromKey(context))) {
                scheduleTypingExpiration();
                emit typingChanged(TypingActivityState::channelFromKey(context),
                                   TypingActivityState::parentFromKey(context));
            }
        } else {
            ++it;
        }
    }
}

void Backend::setTypingViewVisible(QObject* view, const QString& channelId,
                                   const QString& parentId, bool visible)
{
    if (!view || channelId.isEmpty()) {
        return;
    }
    if (visible && !_typingTrackedViews.contains(view)) {
        _typingTrackedViews.insert(view);
        connect(view, &QObject::destroyed, this, [this, view] {
            dropTypingView(view);
            _typingTrackedViews.remove(view);
        });
    }

    // The context is specified by the surface, not by the active global team.
    const QString key = TypingActivityState::contextKey(channelId, parentId);
    if (visible) {
        _typingViews[key].insert(view);
    } else {
        auto it = _typingViews.find(key);
        if (it != _typingViews.end()) {
            it->remove(view);
            if (it->isEmpty()) {
                _typingViews.erase(it);
                _lastTypingSentMs.remove(key);
                if (_typingActivity.clearContext(channelId, parentId)) {
                    scheduleTypingExpiration();
                    emit typingChanged(channelId, parentId);
                }
            }
        }
    }
}

void Backend::scheduleTypingExpiration()
{
    const qint64 next = _typingActivity.nextExpiryMs();
    if (next == std::numeric_limits<qint64>::max()) {
        _typingExpiryTimer.stop();
        return;
    }
    const qint64 remaining = std::max<qint64>(1, next - _typingClock.elapsed());
    _typingExpiryTimer.start(static_cast<int>(std::min<qint64>(remaining, 600000)));
}

void Backend::clearTypingActivity()
{
    _typingExpiryTimer.stop();
    _lastTypingSentMs.clear();
    for (const QString& context : _typingActivity.clear()) {
        emit typingChanged(TypingActivityState::channelFromKey(context),
                           TypingActivityState::parentFromKey(context));
    }
}

QStringList Backend::typingUsers(const QString& channelId,
                                 const QString& parentId) const
{
    return hasVisibleTypingView(channelId, parentId)
        ? _typingActivity.users(channelId, parentId) : QStringList();
}

void Backend::noteUserTyping(const QString& channelId, const QString& parentId,
                             const QString& userId)
{
    if (!isLoggedIn || !storage.loginUser || userId == storage.loginUser->id
        || !hasVisibleTypingView(channelId, parentId)) {
        return;
    }

    const qint64 now = _typingClock.elapsed();
    const int lifetime = std::max(5000, webSocketConnector.typingIntervalMs() + 1800);
    const bool first = _typingActivity.observe(
        channelId, parentId, userId, now, lifetime);
    scheduleTypingExpiration();
    if (first) {
        emit typingChanged(channelId, parentId);
    }
}

void Backend::noteUserPosted(const QString& channelId, const QString& parentId,
                             const QString& userId)
{
    if (_typingActivity.remove(channelId, parentId, userId)) {
        scheduleTypingExpiration();
        emit typingChanged(channelId, parentId);
    }
}

void Backend::publishUserTyping(const QString& channelId, const QString& parentId,
                                int channelMemberCount)
{
    if (!isLoggedIn || !storage.loginUser || channelId.isEmpty()
        || !hasVisibleTypingView(channelId, parentId)
        || !webSocketConnector.typingEnabled()
        || (channelMemberCount >= 0
            && channelMemberCount >= webSocketConnector.typingMaximumMembers())) {
        return;
    }
    const QString key = TypingActivityState::contextKey(channelId, parentId);
    const qint64 now = _typingClock.elapsed();
    const int interval = webSocketConnector.typingIntervalMs();
    if (_lastTypingSentMs.contains(key)
        && now - _lastTypingSentMs.value(key) < interval) {
        return;
    }
    if (webSocketConnector.sendUserTyping(channelId, parentId)) {
        _lastTypingSentMs.insert(key, now);
    }
}

Backend::Backend(QObject *parent)
:QObject (parent)
,_emojiRegistry()
,serverDialogsMap (*this)
,httpConnector(&_networkSession, &storage.localPostDeleteTracker)
,webSocketEventHandler (*this)
,webSocketConnector (webSocketEventHandler)
,currentChannel (nullptr)
,isLoggedIn (false)
,autoLoginEnabledFlag (true)
,nonFilledTeams (0)
,avatarsLoaded(0)
{
    _typingClock.start();
    _typingExpiryTimer.setSingleShot(true);
    connect(&_typingExpiryTimer, &QTimer::timeout, this, [this] {
        for (const QString& context : _typingActivity.expire(_typingClock.elapsed())) {
            emit typingChanged(TypingActivityState::channelFromKey(context),
                               TypingActivityState::parentFromKey(context));
        }
        scheduleTypingExpiration();
    });
    connect(this, &Backend::onWebSocketDisconnect,
            this, &Backend::clearTypingActivity);
	connect (&webSocketConnector, &WebSocketConnector::onConnect,
	         [this] (bool isReconnect, bool needsHttpResync) {
        if (!isLoggedIn) {
            return; // Ignore an in-flight reconnect during logout.
        }
        CustomEmojiService::instance(*this).resetSession();
		if (isReconnect) {
			LOG_DEBUG("WebSocket reconnected - restarting HTTP transport");

			/**
			 * A route/interface change can leave QNetworkAccessManager requests
			 * bound to the dead path even after a fresh WebSocket succeeds.
			 * Replace the HTTP transport before notifying listeners so their
			 * onWebSocketConnect retries are issued on the new network path.
			 */
			HTTPConnector::restartAllTransports();
		}

		emit onWebSocketConnect();

        // WebSocket replay alone does not refresh HTTP-backed projections.
        // Sources reconcile their visible timelines and defer hidden ones.
        if (isReconnect) {
            emit onRealtimeRecovered();
        }

		// Reliable WebSocket replay covers normal reconnects. Only when the
		// server explicitly started a new stream do we need a history fallback.
		if (needsHttpResync && currentChannel) {
			LOG_DEBUG("Reliable WebSocket resume failed - refreshing current channel");
			retrieveChannelPosts(*currentChannel, 0, 25);
		}
	});

	connect(&webSocketConnector, &WebSocketConnector::reliableResumeFailed,
	        this, [this] {
		LOG_DEBUG("Reliable WebSocket resume failed after authentication");
        emit onRealtimeRecovered();
		if (currentChannel) {
			retrieveChannelPosts(*currentChannel, 0, 25);
		}
	});

    connect(this, &Backend::onFlaggedPostChanged, this,
            [this](const QString& postId, bool flagged) {
                if (flagged) _flaggedPostIds.insert(postId);
                else _flaggedPostIds.remove(postId);
            });

	//these signals are proxied
	connect (&webSocketConnector, &WebSocketConnector::onDisconnect, this, &Backend::onWebSocketDisconnect);
    connect(&webSocketConnector, &WebSocketConnector::connectionStateChanged,
            this, &Backend::onWebSocketConnectionStateChanged);
    connect(&webSocketConnector, &WebSocketConnector::connectionStateChanged,
            this, [this](WebSocketConnector::ConnectionState state) {
        if (state != WebSocketConnector::ConnectionState::Connected) {
            clearTypingActivity();
        }
    });
	connect (&httpConnector, &HTTPConnector::onNetworkError, this, &Backend::onNetworkError);

	connect (&httpConnector, &HTTPConnector::onHttpError, [this] (uint32_t errorNumber, const QString& errorText) {

		emit onHttpError (errorNumber, errorText);

		if (errorNumber == 401) {
			if (isLoggedIn) { //invalid session, login again
				QTimer::singleShot (1000, [this] {
					httpConnector.reset ();
					webSocketConnector.reset ();
					loginRetry ();
					exit (1);
				});
			} else {
				NetworkRequest::clearToken ();
			}
		}
	});

	attachmentsCache.setCacheDirectory (QDir (QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath("attachments"));
	attachmentsCache.setMaximumCacheSize (300 * 1024 * 1024);
    // Keep the backend-scoped lazy custom resolver connected before
    // login/post loading so registry misses can never be lost.
    (void)CustomEmojiService::instance(*this);
	(void)RealtimeFallbackService::instance(*this);
}

Backend::~Backend()
{
    // QObject children (account services) normally outlive Backend members
    // until QObject's base destructor. Destroy them while the session, Storage
    // and shared transport still exist, so their own transport destructors
    // cannot reference an already-destroyed NetworkSessionContext.
    HTTPConnector::resetSessionTransports(&_networkSession);
    const auto ownedChildren = children();
    for (QObject* child : ownedChildren) {
        delete child;
    }
}

// Full HTTP header diagnostics are a compile-time opt-in. They may contain
// private server-specific fields even after known authentication headers are
// redacted; never enable this in production builds.
#ifdef MATTERLEAST_DEBUG_UPLOAD_HEADERS
// Legacy detailed request dumper, deliberately excluded from normal builds.
// This can expose credentials and request bodies: use only in local debugging.
void debugRequest(const QNetworkRequest& request, QByteArray data = QByteArray())
{
    qDebug() << request.url().toString();
    const QList<QByteArray> rawHeaderList = request.rawHeaderList();
    for (const QByteArray& rawHeader : rawHeaderList) {
        qDebug() << rawHeader << ":" << request.rawHeader(rawHeader);
    }
    qDebug() << data;
}

namespace {

bool isSensitiveHttpHeader(const QByteArray& name)
{
    const QByteArray lower = name.toLower();
    return lower == "cookie"
        || lower == "set-cookie"
        || lower == "authorization"
        || lower == "proxy-authorization"
        || lower == "x-csrf-token"
        || lower == "x-auth-token"
        || lower == "x-api-key"
        || lower.contains("token")
        || lower.contains("secret")
        || lower.contains("api-key");
}

QString requestHeadersForUploadTrace(const QNetworkRequest& request)
{
    QStringList fields;
    const QList<QByteArray> names = request.rawHeaderList();
    fields.reserve(names.size());
    for (const QByteArray& name : names) {
        const QString value = isSensitiveHttpHeader(name)
            ? QStringLiteral("<redacted>")
            : QString::fromUtf8(request.rawHeader(name));
        fields.push_back(QString::fromLatin1(name) + QLatin1Char('=')
                         + value);
    }
    return fields.join(QStringLiteral("; "));
}

QString replyHeadersForUploadTrace(const QNetworkReply& reply)
{
    QStringList fields;
    const QList<QByteArray> names = reply.rawHeaderList();
    fields.reserve(names.size());
    for (const QByteArray& name : names) {
        const QString value = isSensitiveHttpHeader(name)
            ? QStringLiteral("<redacted>")
            : QString::fromUtf8(reply.rawHeader(name));
        fields.push_back(QString::fromLatin1(name) + QLatin1Char('=')
                         + value);
    }
    return fields.join(QStringLiteral("; "));
}

} // namespace
#endif

void Backend::login (const BackendLoginData& credentials, std::function<void(const QString&)> callback)
{
    const bool restartingAfterFailure = _lastLoginFailureTransient;
    _lastLoginFailureTransient = false;
    loginData = credentials;
    _networkSession.setHost(credentials.domain);
    httpConnector.configureAccountCache(credentials.username);
    // Temporary bridge for callers not yet migrated to Backend::networkSession().
    NetworkRequest::setHost(credentials.domain);
    if (restartingAfterFailure) {
        // There is no WebSocket yet to signal a route change during offline
        // startup. Force fresh HTTP connections for each automatic retry.
        httpConnector.restartTransport();
    }

	if (!credentials.token.isEmpty()) {

		_networkSession.setToken(credentials.token);
        NetworkRequest::setToken(credentials.token);
		NetworkRequest request(_networkSession, "users/me");

		httpConnector.get (request, HttpResponseCallback ([this, callback](const QJsonDocument& data, const QNetworkReply& reply) {
			loginSuccess (data, reply, callback);
		}));


#if 0
		NetworkRequest request1(_networkSession, "users/me/preferences");

		httpConnector.get (request1, [this, callback](QVariant, QByteArray data, const QNetworkReply& reply) {
			QJsonDocument doc = QJsonDocument::fromJson(data);

			QString jsonString = doc.toJson(QJsonDocument::Indented);
			LOG_DEBUG (jsonString);
		});
#endif

		return;
	}

	QJsonDocument  json;
	QJsonObject  jsonRoot;

	jsonRoot.insert("login_id", credentials.username);
	jsonRoot.insert("password", credentials.password);
	//jsonRoot.insert("device_id", "QT Client");
#if 0
	if (!token.isEmpty())
		jsonRoot.insert("token", token);
#endif
	json.setObject(jsonRoot);

    // A password login must not accidentally reuse a previously rejected
    // Bearer/cookie token from a failed saved-session attempt.
    _networkSession.clearToken();
    NetworkRequest::clearToken();
	NetworkRequest request(_networkSession, "users/login");


	httpConnector.post (request, json, HttpResponseCallback ([this, callback](const QJsonDocument& data, const QNetworkReply& reply) {
		loginSuccess (data, reply, callback);
	}));
}

void Backend::cancelPendingLogin()
{
    if (isLoggedIn) return;
    // Invalidate the HTTP generation before aborting replies so a late
    // /users/me response cannot initialize a session after cancellation.
    HTTPConnector::resetSessionTransports(&_networkSession);
    _networkSession.clearToken();
    NetworkRequest::clearToken();
    _lastLoginFailureTransient = false;
}

void Backend::loginRetry ()
{
	if (!loginData.token.isEmpty()) {

		_networkSession.setToken(loginData.token);
        NetworkRequest::setToken(loginData.token);
		NetworkRequest request(_networkSession, "users/me");

		LOG_DEBUG("Retrying authenticated login request");

		httpConnector.get (request, HttpResponseCallback ([this](QVariant, QByteArray, const QNetworkReply&) {
			webSocketConnector.open (_networkSession.host() + "api/v4/", loginData.token);
			isLoggedIn = true;
			//loginSuccess (data, reply, [this] (const QString& token) {
		//	});
		}));

		return;
	}

	LOG_DEBUG ("Login retry - no token");
}

void Backend::setCurrentChannel (BackendChannel& channel)
{
	currentChannel = &channel;
}

BackendChannel* Backend::getCurrentChannel () const
{
	return currentChannel;
}

WebSocketConnector::ConnectionState Backend::webSocketConnectionState() const
{
    return webSocketConnector.connectionState();
}

void Backend::retryWebSocketConnectionNow()
{
    webSocketConnector.reconnectNow();
}

bool Backend::autoLoginEnabled ()
{
	return autoLoginEnabledFlag;
}

void Backend::loginSuccess (const QJsonDocument& doc, const QNetworkReply& reply, std::function<void (const QString&)> callback)
{
    const LoginResponseValidation validation = validateLoginResponse(
        reply.error(),
        reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), doc);
    _lastLoginFailureTransient = validation.retryable;
    if (!validation.accepted) {
        qWarning() << "Login identity fetch failed, retryable="
                   << validation.retryable
                   << "networkError=" << reply.error()
                   << "status=" << reply.attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (callback) callback(QString());
        return;
    }
    QByteArray issuedToken = reply.rawHeader("Token");
    if (_networkSession.token().isEmpty() && issuedToken.isEmpty()) {
        qWarning() << "Login rejected: server returned no session token";
        if (callback) callback(QString());
        return;
    }

    BackendUser* loginUser = storage.addUser(doc.object(), true);
    loginUser->isLoginUser = true;

    if (_networkSession.token().isEmpty()) {
        _networkSession.setToken(QString::fromUtf8(issuedToken));
        NetworkRequest::setToken(QString::fromUtf8(issuedToken));
    }

	webSocketConnector.open (_networkSession.host() + "api/v4/", _networkSession.token());
	isLoggedIn = true;
	retrieveUserPreferences ();

    // Prewarm only the small reaction working set. Built-in names resolve
    // locally; unknown names trigger CustomEmojiService's lazy lookup and disk
    // cache. Do not enumerate/download the server's custom-emoji catalog.
    for (const QString& emojiName :
         ReactionUsageTracker::instance().topNames(10)) {
        (void)_emojiRegistry.resolveByName(emojiName);
    }

	//retrieveAllPublicTeams ();
	if (callback) callback(_networkSession.token());
}

void Backend::reset ()
{
	isLoggedIn = false;
    CustomEmojiService::instance(*this).resetSession();
    _emojiRegistry.clearCustomEmojis();

	/*
	 * This is important. Disconnect all signals. Added lambda functions are not removed when
	 * objects are destroyed
	 */
	disconnect ();
    _flaggedPostIds.clear();
    // Backend::reset() deliberately disconnects every connection, including
    // the internal flagged-preference cache observer. Reinstall that observer
    // for a subsequent login in the same process.
    connect(this, &Backend::onFlaggedPostChanged, this,
            [this](const QString& postId, bool flagged) {
                if (flagged) _flaggedPostIds.insert(postId);
                else _flaggedPostIds.remove(postId);
            });
    _networkSession.clearToken();
	NetworkRequest::clearToken ();

	//reinit all network connectors
	timeoutTimer.disconnect ();
    HTTPConnector::resetSessionTransports(&_networkSession);
	webSocketConnector.close ();
    // Session projections must not retain pointers into the old Storage after
    // logout. Previously currentChannel could still point to a deleted
    // BackendChannel, even once the MainWindow was destroyed.
    currentChannel = nullptr;
    currentTeamContextId.clear();
    clearTypingActivity();
    _typingViews.clear();
    _typingTrackedViews.clear();
	storage.reset ();
	nonFilledTeams = 0;
    PluginManager::restoreBackendConnections(*this);
}

void Backend::prepareForLogout()
{
    if (isLoggedIn) {
        emit sessionEnding();
    }
    isLoggedIn = false;
    autoLoginEnabledFlag = false;

    // First stop the reconnect loop, then cancel outstanding HTTP work while
    // the old MainWindow and its callback targets still exist. HTTPConnector
    // increments its generation before destroying replies, so stale response
    // handlers are suppressed instead of touching destroyed post widgets.
    webSocketConnector.close();
    HTTPConnector::resetSessionTransports(&_networkSession);
}

void Backend::logout(std::function<void()> callback)
{
    NetworkRequest request(_networkSession, QStringLiteral("users/logout"));
    if (isLoggedIn) {
        emit sessionEnding();
    }
    isLoggedIn = false;
    autoLoginEnabledFlag = false;

    // Logout must not race the WebSocket reconnect loop: a late reconnect
    // previously restarted the HTTP transport during session teardown.
    webSocketConnector.close();

    // The server reply and timeout are competing completion paths. The first
    // one owns reset() and callback; an aborted HTTP request may still invoke
    // its completion handler while reset() is in progress.
    const auto completed = QSharedPointer<bool>::create(false);
    const auto finish = [this, completed, callback = std::move(callback)] {
        if (*completed) {
            return;
        }
        *completed = true;
        timeoutTimer.stop();
        reset();
        if (callback) {
            callback();
        }
    };

    timeoutTimer.stop();
    timeoutTimer.disconnect();
    timeoutTimer.setSingleShot(true);
    connect(&timeoutTimer, &QTimer::timeout, this, [finish] {
        LOG_DEBUG("Logout timeout");
        finish();
    });
    timeoutTimer.start(1000);

    httpConnector.post(request, QByteArray(), HttpResponseCallback(
        [finish](const QJsonDocument&) {
        LOG_DEBUG("Logout done");
        finish();
    }));
}

void Backend::retrieveUser (const QString & userID, std::function<void(const BackendUser&)> callback)
{
	NetworkRequest request (_networkSession, "users/" + userID);

	LOG_DEBUG ("retrieveUser " << userID);

	httpConnector.get (request, HttpResponseCallback ([this, callback](const QJsonDocument& doc) {

		LOG_DEBUG ("retrieveUser reply");

		QString jsonString = doc.toJson(QJsonDocument::Indented);
		//std::cout << "get users reply: " << statusCode.toInt() << std::endl;

		BackendUser *user = storage.addUser (doc.object());
		callback (*user);
	}));
}

void Backend::retrieveUserPreferences ()
{
	NetworkRequest request (_networkSession, "users/" + getLoginUser().id + "/preferences");

	httpConnector.get (request, HttpResponseCallback ([this](const QJsonDocument& doc) {

		LOG_DEBUG ("retrieveUserPreferences reply");

        if (!doc.isArray()) return;
        QSet<QString> known;
        for (const QJsonValue& entry : doc.array()) {
            const QJsonObject pref = entry.toObject();
            if (pref.value(QStringLiteral("category")).toString()
                == QLatin1String("flagged_post")
                && pref.value(QStringLiteral("value")).toString()
                    .compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
                const QString id = pref.value(QStringLiteral("name")).toString();
                if (!id.isEmpty()) known.insert(id);
            }
        }
        const QSet<QString> previous = _flaggedPostIds;
        _flaggedPostIds = known;
        for (const QString& id : known) {
            if (!previous.contains(id)) emit onFlaggedPostChanged(id, true);
        }
        for (const QString& id : previous) {
            if (!known.contains(id)) emit onFlaggedPostChanged(id, false);
        }
	}));
}

void Backend::updateUserPreferences (const BackendUserPreferences& preferences)
{
	NetworkRequest request (_networkSession, "users/" + getLoginUser().id + "/preferences");

	QJsonArray jsonArr;
	jsonArr.push_back (QJsonObject {
		{"user_id", getLoginUser().id},
		{"category", preferences.category},
		{"name", preferences.name},
		{"value", preferences.value},
	});

    httpConnector.put(request, jsonArr, HttpResponseCallback(
        [](const QJsonDocument&) {}));
    if (preferences.category == QLatin1String("flagged_post"))
        emit onFlaggedPostChanged(preferences.name,
                                 preferences.value == QLatin1String("true"));
}

void Backend::deleteUserPreferences (const BackendUserPreferences& preferences)
{
    NetworkRequest request(_networkSession, "users/" + getLoginUser().id + "/preferences/delete");

    QJsonArray jsonArr;
    jsonArr.push_back(QJsonObject {
        {"user_id", getLoginUser().id},
        {"category", preferences.category},
        {"name", preferences.name},
        {"value", preferences.value},
    });

    httpConnector.post(request, jsonArr, HttpResponseCallback([](const QJsonDocument&) {}));
    if (preferences.category == QLatin1String("flagged_post"))
        emit onFlaggedPostChanged(preferences.name, false);
}

void Backend::retrieveMultipleUsersStatus (const QVector<QString> & userIDs, std::function<void()> callback)
{
	if (userIDs.isEmpty()) {
		return; //nothing to do
	}

	QJsonArray userIDsJson;

	for (auto& id: userIDs) {
		userIDsJson.push_back (id);
	}

	NetworkRequest request (_networkSession, "users/status/ids");

	httpConnector.post (request, userIDsJson, HttpResponseCallback ([this, callback] (const QJsonDocument& doc) {

		LOG_DEBUG ("retrieveMultipleUsersStatus reply");

		for (const auto& element: doc.array()) {

			const QJsonObject& jsonObject = element.toObject();

			QString userId = jsonObject.value("user_id").toString();;

			BackendUser* user = storage.getUserById (userId);

			if (!user) {
				LOG_DEBUG ("retrieveMultipleUsersStatus: used with id '" << userId << "not found");
				continue;
			}

			user->status = jsonObject.value("status").toString();
			user->lastActivity = jsonObject.value("last_activity_at").toVariant().toULongLong();
			++user->statusRevision;
		}

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		qDebug() << "get user status reply: " << jsonString;
#endif
		callback ();
	}));
}

void Backend::retrieveTotalUsersCount (std::function<void(uint32_t)> callback)
{
	NetworkRequest request (_networkSession, "users/stats");

	httpConnector.get (request, HttpResponseCallback ([this, callback] (const QJsonDocument& doc) {

		LOG_DEBUG ("getTotalUsersCount reply");

		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;

		storage.totalUsersCount = doc.object().value("total_users_count").toInt();
		callback (storage.totalUsersCount);
	}));
}

void Backend::retrieveKnownUsers (std::function<void()> callback){
 	NetworkRequest requestKnown (_networkSession, "users/known");
 	httpConnector.get (requestKnown, HttpResponseCallback ([this, callback] (const QJsonDocument& doc) {
		storage.knownUsers = doc.array();
		callback();
	}));
}

void Backend::retrieveAllUsers ()
{
	const uint32_t usersPerPage = 200;

	uint32_t totalPages = storage.totalUsersCount / usersPerPage + 1;
	static uint32_t obtainedPages;

	for (uint32_t page = 0; page < totalPages; ++page) {
		NetworkRequest request (_networkSession, "users?per_page=" + QString::number(usersPerPage) + "&active=true&page=" + QString::number(page));

		httpConnector.get (request, HttpResponseCallback ([this, page, totalPages, usersPerPage] (const QJsonDocument& doc) {

			LOG_DEBUG ("retrieveAllUsers reply");

#if 0
			QString jsonString = doc.toJson(QJsonDocument::Indented);
			std::cout << jsonString.toStdString() << std::endl;
#endif

			QVector<QString> userIds;
			userIds.reserve (usersPerPage);

			for (const auto &itemRef: doc.array()) {
				BackendUser *user = storage.addUser (itemRef.toObject());
				userIds.push_back (user->id);
			}

			// retrieveMultipleUsersStatus (userIds, [] {
			// });

			LOG_DEBUG ("Page " << page << " (" << obtainedPages << " of " << totalPages
			           << "): users count: " << doc.array().size()
			           << " (total: " << storage.users.size() << ")");
		#if 0
			for (auto& user: users) {
				std::cout << user.id.toStdString() << " "
						<< user.first_name.toStdString() <<	" "
						<< user.last_name.toStdString() <<	" "
						<< user.username.toStdString() <<	" "
						<< std::endl;
			}
		#endif

			++obtainedPages;
			if (obtainedPages == totalPages) {
				emit onAllUsers ();
				LOG_DEBUG ("Get Users: Done ");
				obtainedPages = 0;
				auto it = storage.users.begin(), end = storage.users.end();
				//intersect active and known user sets, just to show correct total number,
				//save deleted known users, to load their info and avatars later
				for (; it != end; ++it){
					if (storage.knownUsers.contains(it->first))
						storage.knownActiveUsers.insert(it->first);
					else
						storage.knownDeletedUsers.insert(it->first);
				}
				storage.knownActiveUsersCount = storage.knownActiveUsers.size();
				storage.knownUsers = QJsonArray();
				//other users avatars are loaded only when needed
				retrieveUserAvatar (getLoginUser().id);
			}
		}));

	}
}

void Backend::retrieveUserAvatar (const QString & userID)
{
	NetworkRequest request (_networkSession, "users/" + userID + "/image", true);
	request.setPriority(QNetworkRequest::LowPriority);

 	request.setAttribute(QNetworkRequest::BackgroundRequestAttribute, true);
	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
	//LOG_DEBUG ("getUserImage request");

	httpConnector.get (request, HttpResponseCallback ([this, userID] (QVariant, QByteArray data) {
		
		//LOG_DEBUG ("getUserImage reply");

		BackendUser* user = storage.getUserById (userID);

		if (!user) {
			qCritical() << "Get Image: user " << userID << " not found";
			return;
		}

		QPixmap pixmap = decodeAvatarImage(data);
		if (pixmap.isNull()) {
			return;
		}
		user->avatar = std::move(pixmap);
		user->avatar_picture_update = user->last_picture_update;

		emit user->onAvatarChanged();

	}));
}

void Backend::retrieveFile (const QString & fileID, std::function<void(const QByteArray&)> callback)
{
	NetworkRequest request (_networkSession, "files/" + fileID, true);

	QIODevice* cachedFile = attachmentsCache.data (fileID);

	if (cachedFile) {
		//LOG_DEBUG ("Retrieve File " << fileID << " done (from custom cache). Attachment cache size: " << attachmentsCache.cacheSize());

		/**
		 * Do not call callback in the same stack frame
		 * so that the same behavior is achieved like non-cached files.
		 * The behavior may matter in cases like widget resize, when the file is received
		 */
		QTimer::singleShot(1, [callback, cachedFile] {
			callback (cachedFile->readAll());
			cachedFile->close();
			delete cachedFile;
		});
		return;
	}

	QNetworkCacheMetaData metaData;
	metaData.setUrl(fileID);
	QIODevice* cacheIO = attachmentsCache.prepare(metaData);

	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
	httpConnector.get (request, HttpResponseCallback ([this, fileID, callback, cacheIO](QVariant, QByteArray data) {
		//LOG_DEBUG ("Retrieve File " << fileID << " done");
		cacheIO->write (data);
		attachmentsCache.insert (cacheIO);
		callback (data);
	}));
}

/**
 * Get a list of teams that a user is on.
 */
void Backend::retrieveOwnTeams (std::function<void(BackendTeam&)> callback)
{
    NetworkRequest request (_networkSession, "users/me/teams");
    //request.setRawHeader("X-Requested-With", "XMLHttpRequest");

    LOG_DEBUG ("retrieveOwnTeams request");

    httpConnector.get (request, HttpResponseCallback ([this, callback] (
            const QJsonDocument& doc, const QNetworkReply& reply) {
        if (reply.error() != QNetworkReply::NoError || !doc.isArray()) {
            LOG_DEBUG("retrieveOwnTeams failed; retrying in 5 seconds");
            QTimer::singleShot(5000, this, [this, callback] {
                if (isLoggedIn) retrieveOwnTeams(callback);
            });
            return;
        }
    	LOG_DEBUG ("retrieveOwnTeams reply");
		storage.teams.clear ();
		nonFilledTeams = 0;

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << "get teams reply: "  << std::endl;
		std::cout << jsonString.toStdString() << std::endl;
#endif

		 auto root = doc.array();
		for (const auto &itemRef: std::as_const(root)) {
			storage.addTeam (itemRef.toObject());
			++nonFilledTeams;
		}

		for (auto& team: storage.teams) {
			callback (team.second);
		}
        if (storage.teams.empty()) {
            emit onAllTeamChannelsPopulated();
        }
    }));
}

void Backend::retrieveAllPublicTeams ()
{
    NetworkRequest request (_networkSession, "teams");

    LOG_DEBUG ("retrieveOwnTeams request");

    httpConnector.get (request, HttpResponseCallback ([this] (const QJsonDocument& doc) {
    	LOG_DEBUG ("retrieveAllPublicTeams reply");
#if 1
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif
    }));
}

void Backend::retrieveTeam (QString teamID)
{
	NetworkRequest request (_networkSession, "teams/" + teamID);

	LOG_DEBUG ("retrieveTeam '" << teamID << "'");

    httpConnector.get (request, HttpResponseCallback ([this] (const QJsonDocument& doc) {

#if 1
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << "get team reply: " <<  jsonString.toStdString() << std::endl;
#endif

		auto object = doc.object();
		BackendTeam *team = storage.addTeam (doc.object());

		if (team) {
			emit onAddedToTeam (*team);
		}
    }));
}

void Backend::retrieveTeamPublicChannelsPage(
    QString teamID,
    int page,
    int perPage,
    std::function<void(QJsonArray)> callback)
{
    NetworkRequest request(_networkSession, publicChannelsPagePath(teamID, page, perPage));
    httpConnector.get(request, HttpResponseCallback(
        [callback = std::move(callback)](QVariant, const QJsonDocument& doc) mutable {
            if (callback) {
                callback(doc.array());
            }
        }));
}

void Backend::searchTeamPublicChannels(
    QString teamID,
    QString term,
    std::function<void(QJsonArray)> callback)
{
    NetworkRequest request(_networkSession, QStringLiteral("teams/") + teamID
                           + QStringLiteral("/channels/search"));
    const QJsonObject payload {{QStringLiteral("term"), std::move(term)}};
    httpConnector.post(request, QByteArrayCreator(payload),
                       HttpResponseCallback(
        [callback = std::move(callback)](const QJsonDocument& doc) mutable {
            if (callback) {
                callback(doc.array());
            }
        }));
}

void Backend::retrieveChannelsMemberCounts(
    const QStringList& channelIds,
    std::function<void(QJsonObject)> callback)
{
    QJsonArray payload;
    for (const QString& channelId : channelIds) {
        if (!channelId.isEmpty()) {
            payload.append(channelId);
        }
    }

    if (payload.isEmpty()) {
        if (callback) {
            callback({});
        }
        return;
    }

    NetworkRequest request(_networkSession, QStringLiteral("channels/stats/member_count"));
    httpConnector.post(request, QByteArrayCreator(payload),
                       HttpResponseCallback(
        [callback = std::move(callback)](const QJsonDocument& doc) mutable {
            if (callback) {
                callback(doc.object());
            }
        }));
}

void Backend::retrieveOwnChannelMembershipsForTeam (BackendTeam& team, std::function<void(BackendChannel&)> callback)
{
    NetworkRequest request (_networkSession, "users/me/teams/" + team.id + "/channels");
    QPointer<BackendTeam> teamGuard(&team);

    httpConnector.get (request, HttpResponseCallback ([this, teamGuard, callback] (
            const QJsonDocument& doc, const QNetworkReply& reply) {
        if (reply.error() != QNetworkReply::NoError || !doc.isArray()) {
            LOG_DEBUG("Channel memberships request failed; retrying in 5 seconds");
            QTimer::singleShot(5000, this, [this, teamGuard, callback] {
                if (isLoggedIn && teamGuard)
                    retrieveOwnChannelMembershipsForTeam(*teamGuard, callback);
            });
            return;
        }
        // A refreshed team snapshot may replace this QObject while HTTP is in
        // flight, even reusing its ID/address. Never apply the old response to
        // a destroyed team or decrement the new snapshot's startup counter.
        if (!teamGuard) return;
        BackendTeam& currentTeam = *teamGuard;
        currentTeam.channels.clear ();

#if 0
    	QString jsonString = doc.toJson(QJsonDocument::Indented);
    	std::cout << "retrieveOwnChannelMembershipsForTeam reply: " <<  jsonString.toStdString() << std::endl;
#endif

        LOG_DEBUG ("Team " << currentTeam.display_name << ":");
		for (const auto &itemRef: doc.array()) {
			storage.addChannel (currentTeam, itemRef.toObject());
		}

		for (auto& channel: currentTeam.channels) {
			callback (*channel.get());
			LOG_DEBUG ("\tChannel added: " << channel->id << " " << channel->display_name);
		}

		--nonFilledTeams;

		if (nonFilledTeams == 0) {
			emit onAllTeamChannelsPopulated ();
		}
    }));
}

#if 0 //supported in Mattermost server 6.2
void Backend::retrieveOwnAllChannelMemberships (std::function<void ()> callback)
{
    QNetworkRequest request;

    request.setUrl(QUrl(httpHost + "users/me/channel_members?page=0&per_page=2"));
    request.setRawHeader("X-Requested-With", "XMLHttpRequest");

    httpConnector.get(request, [this, callback](QVariant, QByteArray data) {

    	QJsonDocument doc = QJsonDocument::fromJson(data);

    	QString jsonString = doc.toJson(QJsonDocument::Indented);
    	std::cout << "getOwnAllChannelMemberships reply: " <<  jsonString.toStdString() << std::endl;


		callback ();
    });
}
#endif

void Backend::retrieveTeamMembers (BackendTeam& team, int page)
{
	static constexpr int itemsPerPage = 60;
	NetworkRequest request (_networkSession, "teams/" + team.id + "/members?page=" + QString::number(page) + "&per_page=" + QString::number (itemsPerPage));

	//LOG_DEBUG ("retrieveTeamMembers " << team.display_name << " page " << page);

	httpConnector.get(request, HttpResponseCallback ([this, &team, page] (const QJsonDocument& doc) {

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << "retrieveTeamMembers reply: " <<  jsonString.toStdString() << std::endl;
#endif
		auto root = doc.array();
		for (const auto &itemRef: std::as_const(root)) {
			team.addMember (storage, itemRef.toObject());
		}

		//there may be more pages
		if (root.size() == itemsPerPage) {
			retrieveTeamMembers (team, page + 1);
		}
	}));
}

void Backend::retrieveTeamMember (BackendTeam& team, const BackendUser& user)
{
	NetworkRequest request (_networkSession, "teams/" + team.id + "/members/" + user.id);
	RequestTrackerEntry trackedEntry (RequestTrackerID::teamMember, &team, &user);

	if (requestTracker.hasEntry (trackedEntry)) {
		LOG_DEBUG ("retrieveTeamMember for team '" << team.name << "', user '" << user.getDisplayName() << "': skipped because of existing request");
		return;
	}

	LOG_DEBUG ("retrieveTeamMember for team '" << team.name << "', user '" << user.getDisplayName() << "'");

	requestTracker.addEntry (trackedEntry);

	httpConnector.get (request, HttpResponseCallback ([this, &team, &user, trackedEntry](const QJsonDocument& doc) {

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << "retrieveTeamMember reply: " <<  jsonString.toStdString() << std::endl;
#endif
		team.addMember (storage, doc.object());
		requestTracker.eraseEntry (trackedEntry);
		emit (team.onUserAdded (user));
	}));
}

void Backend::retrieveChannel (BackendTeam& team,
                               QString channelID,
                               std::function<void(BackendChannel&)> callback)
{
    NetworkRequest request(_networkSession, "channels/" + channelID);
    LOG_DEBUG("retrieveChannel '" << channelID << "' of team '" << team.name << "'");

    QPointer<BackendTeam> teamGuard(&team);
    httpConnector.get(request, HttpResponseCallback(
        [this, teamGuard, channelID, callback = std::move(callback)]
        (const QJsonDocument& doc, const QNetworkReply& reply) mutable {
        if (!teamGuard) {
            return;
        }

        const int httpStatus =
            reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply.error() != QNetworkReply::NoError
            || httpStatus < 200 || httpStatus >= 300 || !doc.isObject()) {
            qWarning() << "retrieveChannel failed:" << channelID
                       << "HTTP" << httpStatus << "network error" << reply.error();
            return;
        }

        const QJsonObject channelData = doc.object();
        const auto channelType = BackendChannel::getChannelType(channelData);
        if (channelData.value("id").toString() != channelID
            || (channelType != BackendChannel::publicChannel
                && channelType != BackendChannel::privateChannel)) {
            qWarning() << "retrieveChannel: invalid channel data for" << channelID;
            return;
        }

        BackendChannel* channel = storage.addTeamChannel(*teamGuard, channelData);
        if (!channel) {
            channel = storage.getChannelById(channelID);
        }
        if (!channel) {
            return;
        }

        LOG_DEBUG("\tChannel available: " << channel->id << " " << channel->display_name);
        emit teamGuard->onNewChannel(*channel);
        if (callback) {
            callback(*channel);
        }
    }));
}

void Backend::retrieveDirectChannel (QString channelID)
{
    NetworkRequest request(_networkSession, "channels/" + channelID);
    LOG_DEBUG("retrieveDirectChannel " << channelID);

    httpConnector.get(request, HttpResponseCallback(
        [this, channelID](const QJsonDocument& doc, const QNetworkReply& reply) {
        const int httpStatus =
            reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply.error() != QNetworkReply::NoError
            || httpStatus < 200 || httpStatus >= 300 || !doc.isObject()) {
            qWarning() << "retrieveDirectChannel failed:" << channelID
                       << "HTTP" << httpStatus << "network error" << reply.error();
            return;
        }

        const QJsonObject channelData = doc.object();
        if (channelData.value("id").toString() != channelID
            || BackendChannel::getChannelType(channelData)
                != BackendChannel::directChannel) {
            qWarning() << "retrieveDirectChannel: invalid channel data for" << channelID;
            return;
        }

        BackendChannel* channel = storage.addDirectChannel(channelData);
        if (!channel) {
            qWarning() << "retrieveDirectChannel: rejected invalid channel" << channelID;
            return;
        }

        LOG_DEBUG("\tNew Channel added: " << channel->id << " " << channel->display_name);
        emit storage.directChannels.onNewChannel(*channel);

        // Match official client visibility for opened direct messages.
        updateUserPreferences(BackendUserPreferences {"direct_channel_show", channel->name, "true"});
    }));
}

void Backend::retrieveChannelPosts (BackendChannel& channel, int page, int perPage)
{
    NetworkRequest request (_networkSession, "channels/" + channel.id + "/posts?page=" + QString::number(page) + "&per_page=" + QString::number(perPage));
    //LOG_DEBUG ("retrieveChannelPosts request for " << channel.display_name << " (" << channel.id << ")");

    httpConnector.get (request, HttpResponseCallback ([this, &channel](const QJsonDocument& doc) {

		//LOG_DEBUG ("retrieveChannelPosts reply for " << channel.display_name << " (" << channel.id << ")");

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif

		QJsonObject root = doc.object();
		channel.addPosts (root.value("order").toArray(), root.value("posts").toObject());
		retrieveChannelPinnedPosts (channel);
    }));
}

void Backend::retrieveChannelPinnedPosts (BackendChannel& channel)
{
    NetworkRequest request (_networkSession, "channels/" + channel.id + "/pinned");
    //LOG_DEBUG ("retrieveChannelPinnedPosts request for " << channel.display_name << " (" << channel.id << ")");

    httpConnector.get (request, HttpResponseCallback ([this, &channel](const QJsonDocument& doc) {

		//LOG_DEBUG ("retrieveChannelPinnedPosts reply for " << channel.display_name << " (" << channel.id << ")");
		//QString jsonString = doc.toJson(QJsonDocument::Indented);
		//std::cout << jsonString.toStdString() << std::endl;

		QJsonObject root = doc.object();
		channel.addPinnedPosts (root.value("order").toArray(), root.value("posts").toObject());
    }));
}

void Backend::retrieveChannelOlderPosts (BackendChannel& channel, int perPage)
{
	if (channel.posts.empty()) {
		LOG_DEBUG ("retrieveChannelOlderPosts skipped for empty channel " << channel.display_name << " (" << channel.id << ")");
		return;
	}

    NetworkRequest request (_networkSession, "channels/" + channel.id + "/posts?page=" + QString::number(0) + "&per_page=" + QString::number(perPage) + "&before=" + channel.posts.front().id);

    httpConnector.get (request, HttpResponseCallback ([this, &channel](const QJsonDocument& doc) {

		LOG_DEBUG ("retrieveChannelOlderPosts reply for " << channel.display_name << " (" << channel.id << ") - since " << channel.posts.front().id);

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif

		QJsonObject root = doc.object();
		channel.prependPosts (root.value("order").toArray(), root.value("posts").toObject());
    }));
}

void Backend::retrieveChannelUnreadPost(
    BackendChannel& channel, std::function<void(const QString&)> responseHandler,
    bool rootsOnly)
{
    NetworkRequest request(_networkSession, "users/me/channels/" + channel.id
                           + "/posts/unread?limit_before=0&limit_after=1");

    httpConnector.get(request, HttpResponseCallback(
        [this, &channel, responseHandler = std::move(responseHandler), rootsOnly](
            const QJsonDocument& doc) {

		//LOG_DEBUG ("retrieveChannelUnreadPost reply for " << channel.display_name << " (" << channel.id << ")");

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif

        const QString firstUnreadPost =
            selectChannelUnreadPostId(doc.object(), rootsOnly);

		responseHandler (firstUnreadPost);
		if (!firstUnreadPost.isEmpty()) {
			emit onUnreadPostsAtStartup (channel);
		}
    }));
}

void Backend::retrieveChannelMember (BackendChannel& channel, const BackendUser& user)
{
	NetworkRequest request (_networkSession, "channels/" + channel.id + "/members/" + user.id);
	RequestTrackerEntry trackedEntry (RequestTrackerID::channelMember, &channel, &user);

	if (requestTracker.hasEntry (trackedEntry)) {
		LOG_DEBUG ("retrieveChannelMember for channel '" << channel.name << "', user '" << user.getDisplayName() << "': skipped because of existing request");
		return;
	}

	LOG_DEBUG ("retrieveChannelMember for channel '" << channel.name << "', user '" << user.getDisplayName() << "'");

	requestTracker.addEntry (trackedEntry);

	httpConnector.get (request, HttpResponseCallback ([this, &channel, &user, trackedEntry](const QJsonDocument& doc) {

#if 0
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << "retrieveChannelMember reply: " <<  jsonString.toStdString() << std::endl;
#endif
		channel.addMember (storage, doc.object());
		requestTracker.eraseEntry (trackedEntry);
		emit (channel.onUserAdded (user));
	}));
}

void Backend::retrievePollMetadata (BackendPoll& poll)
{
	NetworkRequest request (_networkSession, NetworkRequest::matterpoll, "polls/" + poll.id + "/metadata");

	LOG_DEBUG ("retrievePollMetadata request");

	QPointer<BackendPoll> pollGuard(&poll);
	httpConnector.get (request, HttpResponseCallback ([pollGuard](const QJsonDocument& doc) {
		if (!pollGuard) {
			return;
		}

		LOG_DEBUG ("retrievePollMetadata reply");
		pollGuard->fillMetadata (doc.object());
	}));
}

void Backend::markChannelAsViewed (BackendChannel& channel)
{
	QJsonObject  json;

	json.insert ("channel_id", channel.id);

	//maybe add prev_channel_id, the Mattermost API supports it
	//json.insert ("prev_channel_id", channel.id);

	NetworkRequest request (_networkSession, "channels/members/me/view");

	httpConnector.post (request, json, HttpResponseCallback ([this](QVariant, QByteArray) {

		//no callbacks are required. the server will send a WebSocket packet 'channel_viewed'

#if 0
		QJsonDocument doc = QJsonDocument::fromJson(data);
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif

	}));
}

void Backend::editChannelProperties (BackendChannel& channel, const BackendChannelProperties& newProperties)
{
	QJsonObject  json {
		{"id", channel.id},
		{"name", channel.name},
		{"display_name", newProperties.displayName},
		{"purpose", newProperties.purpose},
		{"header", newProperties.header},
	};

	QString jsonString = QJsonDocument(json).toJson(QJsonDocument::Indented);
	std::cout << jsonString.toStdString() << std::endl;

	QByteArray data (QJsonDocument (json).toJson(QJsonDocument::Compact));

	NetworkRequest request (_networkSession, "channels/" + channel.id);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

	httpConnector.put (request, data, HttpResponseCallback ([this](QVariant, QByteArray) {
#if 0
	QJsonDocument doc = QJsonDocument::fromJson(data);
	QString jsonString = doc.toJson(QJsonDocument::Indented);
	std::cout << "editChannelProperties" << jsonString.toStdString() << std::endl;
#endif

	}));
}

void Backend::addPost (BackendChannel& channel, const QString& message, const QList<QString>& attachments, const QString& rootID)
{
	QJsonArray files;

	for (auto& id: attachments) {
		files.push_back (id);
	}

	QJsonObject  json;

	json.insert ("channel_id", channel.id);
	json.insert ("message", message);

	if (!files.isEmpty()) {
		json.insert ("file_ids", files);
	}

	if (!rootID.isEmpty()) {
		json.insert ("root_id", rootID);
	}


	NetworkRequest request (_networkSession, "posts");

	httpConnector.post (request, json, HttpResponseCallback ([this](QVariant, QByteArray) {
#if 0
		QJsonDocument doc = QJsonDocument::fromJson(data);
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif
	}));
}

void Backend::editPost (const QString& postID, const QString& message, const QList<QString>& attachments)
{
	QJsonArray files;

	for (auto& id: attachments) {
		files.push_back (id);
	}

	QJsonObject  json;

	json.insert ("message", message);

	if (!files.isEmpty()) {
		json.insert ("file_ids", files);
	}

	QString jsonString = QJsonDocument(json).toJson(QJsonDocument::Indented);
	std::cout << jsonString.toStdString() << std::endl;

    QByteArray data (QJsonDocument (json).toJson(QJsonDocument::Compact));

	NetworkRequest request (_networkSession, "posts/" + postID + "/patch");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

	httpConnector.put (request, data, HttpResponseCallback ([this](const QJsonDocument& doc) {

#if 1
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif

	}));
}

void Backend::deletePost (const QString postID)
{
	NetworkRequest request (_networkSession, "posts/" + postID);

	httpConnector.del (request);
}

void Backend::pinPost(const QString& postID, const QString& channelID)
{
	NetworkRequest request(_networkSession, 
	    QStringLiteral("posts/") + postID + QStringLiteral("/pin"));
	httpConnector.post(
	    request, QByteArray(),
	    HttpResponseCallback([this, postID, channelID](const QJsonDocument&) {
		    BackendChannel* channel = storage.getChannelById(channelID);
		    if (!channel) {
			    return;
		    }
		    channel->setPostPinned(postID, true);
		    retrieveChannelPinnedPosts(*channel);
	    }));
}

void Backend::unpinPost(const QString& postID, const QString& channelID)
{
	NetworkRequest request(_networkSession, 
	    QStringLiteral("posts/") + postID + QStringLiteral("/unpin"));
	httpConnector.post(
	    request, QByteArray(),
	    HttpResponseCallback([this, postID, channelID](const QJsonDocument&) {
		    BackendChannel* channel = storage.getChannelById(channelID);
		    if (!channel) {
			    return;
		    }
		    channel->setPostPinned(postID, false);
		    retrieveChannelPinnedPosts(*channel);
	    }));
}

void Backend::addPoll (BackendChannel& channel, const BackendNewPollData& pollData)
{
	NetworkRequest request (_networkSession, "actions/dialogs/submit");

	QJsonObject json {
		{"callback_id", ""},
		{"channel_id", channel.id},
		{"state", ""},
		{"url", "/plugins/com.github.matterpoll.matterpoll/api/v1/polls/create"},
		{"team_id", channel.team->id}
	};

	auto submission = QJsonObject {
		{"question", pollData.question}
	};

	for (int i = 0; i < pollData.options.size(); ++i) {
		submission.insert ("option" + QString::number(i + 1), pollData.options[i]);
	}

	if (pollData.isAnonymous) {
		submission.insert ("setting-anonymous", true);
	}

	if (pollData.showProgress) {
		submission.insert ("setting-progress", true);
	}

	if (pollData.allowAddOptions) {
		submission.insert ("setting-public-add-option", true);
	}

	json.insert ("submission", submission);

	httpConnector.post (request, json, HttpResponseCallback ([this](QVariant, QByteArray) {
#if 0
		QJsonDocument doc = QJsonDocument::fromJson(data);
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif
	}));
	return;
}

void Backend::addPostReaction(const QString& postID, const QString& emojiName)
{
	QJsonObject json {
		{"user_id", getLoginUser().id},
		{"post_id", postID},
		{"emoji_name", emojiName},
		{"create_at", 0},
	};

	NetworkRequest request(_networkSession, "reactions");
	httpConnector.post(request, json, HttpResponseCallback([](QVariant, QByteArray) {
		// The authoritative model update arrives as reaction_added.
	}));
}

void Backend::removePostReaction(const QString& postID, const QString& emojiName)
{
    const QByteArray encodedEmoji = QUrl::toPercentEncoding(emojiName);
    NetworkRequest request(_networkSession, 
        QStringLiteral("users/") + getLoginUser().id
        + QStringLiteral("/posts/") + postID
        + QStringLiteral("/reactions/")
        + QString::fromLatin1(encodedEmoji));
    httpConnector.del(request);
}

void Backend::sendPostAction (const BackendPost& post, const QString& action)
{
	NetworkRequest request  (_networkSession, "posts/" + post.id + "/actions/" + action);

	httpConnector.post (request, QByteArray(), HttpResponseCallback ([this](const QJsonDocument& doc) {

#if 1
		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << "sendPostAction reply: " << jsonString.toStdString() << std::endl;

		const QJsonObject& obj = doc.object();
		if (obj.value("status").toString() == "OK") {
			serverDialogsMap.addEvent (obj.value ("trigger_id").toString());
		}
#endif

	}));
}

void Backend::uploadFile(
    BackendChannel& channel,
    const QString& filePath,
    std::function<void(QString, QString)> responseHandler,
    HTTPConnector::UploadProgressHandler progressHandler)
{
    const QFileInfo fileInfo(filePath);
    auto multipart =
        QSharedPointer<QHttpMultiPart>::create(QHttpMultiPart::FormDataType);

    QHttpPart channelPart;
    channelPart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QStringLiteral("form-data; name=\"channel_id\""));
    channelPart.setBody(channel.id.toUtf8());
    multipart->append(channelPart);

    const QString clientId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QHttpPart clientIdPart;
    clientIdPart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QStringLiteral("form-data; name=\"client_ids\""));
    clientIdPart.setBody(clientId.toUtf8());
    multipart->append(clientIdPart);

    // Mattermost's streaming multipart parser expects metadata first and the
    // file part last. This is also the order used by the official web client.
    auto* file = new QFile(filePath, multipart.data());
    if (!file->open(QIODevice::ReadOnly)) {
        const QString error = file->errorString();
        qWarning() << "Cannot open attachment" << filePath << ':' << error;
        if (responseHandler) {
            responseHandler(QString(), error);
        }
        return;
    }

    QString escapedName = fileInfo.fileName();
    escapedName.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escapedName.replace(QLatin1Char('"'), QStringLiteral("\\\""));

    QHttpPart filePart;
    filePart.setHeader(
        QNetworkRequest::ContentDispositionHeader,
        QStringLiteral("form-data; name=\"files\"; filename=\"%1\"")
            .arg(escapedName));

    const QString mimeType =
        QMimeDatabase().mimeTypeForFile(
            filePath, QMimeDatabase::MatchExtension).name();
    if (!mimeType.isEmpty()) {
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
    }
    filePart.setBodyDevice(file);
    multipart->append(filePart);

    NetworkRequest request(_networkSession, QStringLiteral("files"));
    request.setRawHeader("Accept", "application/json");

    const QString connectionId = webSocketConnector.connectionId();
    if (!connectionId.isEmpty()) {
        request.setRawHeader("Connection-Id", connectionId.toUtf8());
    }

    qCInfo(lcUploadTrace).nospace()
        << "UPLOAD_PREPARE clientId=" << clientId
        << " channel=" << channel.id
        << " file=" << fileInfo.fileName()
        << " bytes=" << fileInfo.size()
        << " mime=" << mimeType
        << " endpoint=files"
        << " hasSessionCookie=" << !request.rawHeader("Cookie").isEmpty()
        << " hasConnectionId=" << !connectionId.isEmpty();

    httpConnector.post(
        request,
        multipart,
        HttpResponseCallback(
            [responseHandler = std::move(responseHandler),
             clientId,
             fileName = fileInfo.fileName()](
                QVariant status, QByteArray data, const QNetworkReply& reply) mutable {
                const int httpStatus =
                    reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QSslConfiguration ssl = reply.sslConfiguration();
                qCInfo(lcUploadTrace).nospace()
                    << "UPLOAD_REPLY clientId=" << clientId
                    << " file=" << fileName
                    << " networkError=" << status.toInt()
                    << " http=" << httpStatus
                    << " networkError=" << status.toInt()
                    << " contentType=" << reply.header(QNetworkRequest::ContentTypeHeader).toString()
                    << " responseBytes=" << data.size()
                    << " qt=" << qVersion()
                    << " sslBuild=" << QSslSocket::sslLibraryBuildVersionString()
                    << " sslRuntime=" << QSslSocket::sslLibraryVersionString()
                    << " tlsProtocol=" << static_cast<int>(ssl.sessionProtocol())
                    << " cipher=" << ssl.sessionCipher().name()
                    << " http2="
                    << reply.attribute(QNetworkRequest::Http2WasUsedAttribute)
                           .toBool();

#ifdef MATTERLEAST_DEBUG_UPLOAD_HEADERS
                qCInfo(lcUploadTrace).nospace()
                    << "UPLOAD_REQUEST_HEADERS clientId=" << clientId
                    << " headers={"
                    << requestHeadersForUploadTrace(reply.request())
                    << "}";

                qCInfo(lcUploadTrace).nospace()
                    << "UPLOAD_RESPONSE_HEADERS clientId=" << clientId
                    << " headers={"
                    << replyHeadersForUploadTrace(reply)
                    << "}";
#else
                qCDebug(lcUploadTrace).nospace()
                    << "UPLOAD_HEADER_COUNTS clientId=" << clientId
                    << " request=" << reply.request().rawHeaderList().size()
                    << " response=" << reply.rawHeaderList().size();
#endif

                const QJsonDocument document = QJsonDocument::fromJson(data);
                if (status.toInt() != QNetworkReply::NoError) {
                    QString error = reply.errorString();
                    if (document.isObject()) {
                        const QString serverMessage =
                            document.object().value("message").toString();
                        if (!serverMessage.isEmpty()) {
                            error = serverMessage;
                        }
                    }

                     qCWarning(lcUploadTrace).nospace()
                        << "UPLOAD_FAILED clientId=" << clientId
                        << " file=" << fileName
                        << " http=" << httpStatus
                        << " networkError=" << status.toInt();

                    if (responseHandler) {
                        responseHandler(QString(), error);
                    }
                    return;
                }

                const QJsonArray infos =
                    document.object().value("file_infos").toArray();
                if (infos.isEmpty()) {
                    qCWarning(lcUploadTrace).nospace()
                        << "UPLOAD_INVALID_REPLY clientId=" << clientId
                        << " file=" << fileName
                        << " reason=no-file-infos";
                    if (responseHandler) {
                        responseHandler(
                            QString(),
                            QStringLiteral("Upload returned no file information"));
                    }
                    return;
                }

                const QString fileId =
                    infos.first().toObject().value("id").toString();
                if (fileId.isEmpty()) {
                    qCWarning(lcUploadTrace).nospace()
                        << "UPLOAD_INVALID_REPLY clientId=" << clientId
                        << " file=" << fileName
                        << " reason=empty-file-id";
                    if (responseHandler) {
                        responseHandler(
                            QString(),
                            QStringLiteral("Upload returned an empty file id"));
                    }
                    return;
                }

                qCInfo(lcUploadTrace).nospace()
                    << "UPLOAD_ACCEPTED clientId=" << clientId
                    << " file=" << fileName
                    << " fileId=" << fileId;

                if (responseHandler) {
                    responseHandler(fileId, QString());
                }
            }),
        std::move(progressHandler));
}

void Backend::createChannel(BackendTeam& team,
                            const QString& name,
                            const QString& displayName,
                            const QString& purpose,
                            bool privateChannel,
                            std::function<void(BackendChannel&)> callback)
{
    QJsonObject payload {
        {QStringLiteral("team_id"), team.id},
        {QStringLiteral("name"), name},
        {QStringLiteral("display_name"), displayName},
        {QStringLiteral("purpose"), purpose},
        {QStringLiteral("type"), privateChannel ? QStringLiteral("P")
                                                : QStringLiteral("O")},
    };

    QPointer<BackendTeam> teamGuard(&team);
    NetworkRequest request(_networkSession, QStringLiteral("channels"));
    httpConnector.post(request, QByteArrayCreator(payload), HttpResponseCallback(
        [this, teamGuard, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            if (status.toInt() != QNetworkReply::NoError || !teamGuard
                || !doc.isObject()) {
                return;
            }

            BackendChannel* channel = storage.addTeamChannel(*teamGuard, doc.object());
            if (!channel) {
                return;
            }

            emit teamGuard->onNewChannel(*channel);
            if (callback) {
                callback(*channel);
            }
        }));
}

void Backend::createDirectChannel(const BackendUser& user,
                                  std::function<void(BackendChannel&)> callback)
{
    createDirectChannelDetailed(
        user, [callback = std::move(callback)](
                  BackendChannel* channel, const QString&) mutable {
            if (channel && callback) {
                callback(*channel);
            }
        });
}

void Backend::createDirectChannelDetailed(
    const BackendUser& user,
    std::function<void(BackendChannel*, const QString&)> callback)
{
    if (user.id.isEmpty() || getLoginUser().id.isEmpty()) {
        if (callback) {
            callback(nullptr, tr("Cannot create a direct conversation without user IDs."));
        }
        return;
    }

    QJsonArray json {getLoginUser().id, user.id};
    NetworkRequest request(_networkSession, QStringLiteral("channels/direct"));
    httpConnector.post(request, json, HttpResponseCallback(
        [this, callback = std::move(callback)](
            QVariant status, QByteArray response, const QNetworkReply& reply) mutable {
        const QJsonDocument doc = QJsonDocument::fromJson(response);
        const int httpStatus = reply.attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status.toInt() != QNetworkReply::NoError || httpStatus < 200
            || httpStatus >= 300 || !doc.isObject()) {
            if (callback) {
                const QString serverError = doc.object().value(
                    QStringLiteral("message")).toString();
                callback(nullptr, !serverError.isEmpty()
                    ? serverError : reply.errorString());
            }
            return;
        }

        const QString id = doc.object().value(QStringLiteral("id")).toString();
        const bool alreadyKnown = !id.isEmpty() && storage.getChannelById(id);
        BackendChannel* channel = storage.addDirectChannel(doc.object());
        if (!channel) {
            if (callback) {
                callback(nullptr, tr("The server returned an invalid direct channel."));
            }
            return;
        }

        LOG_DEBUG("\tNew Channel added: " << channel->id << " " << channel->display_name);
        if (!alreadyKnown) {
            emit storage.directChannels.onNewChannel(*channel);
        }

        updateUserPreferences(BackendUserPreferences {
            QStringLiteral("direct_channel_show"), channel->name, QStringLiteral("true")});

        if (callback) {
            callback(channel, {});
        }
    }));
}

void Backend::createGroupChannel(
    const QStringList& requestedUserIds,
    std::function<void(BackendChannel&)> callback)
{
    QStringList userIds = requestedUserIds;
    userIds.removeAll(QString());
    userIds.removeDuplicates();
    userIds.removeAll(getLoginUser().id);

    QJsonArray payload;
    for (const QString& userId : std::as_const(userIds)) {
        payload.append(userId);
    }
    if (payload.isEmpty()) {
        return;
    }

    NetworkRequest request(_networkSession, QStringLiteral("channels/group"));
    httpConnector.post(request, QByteArrayCreator(payload), HttpResponseCallback(
        [this, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
                return;
            }

            const QString channelId =
                doc.object().value(QStringLiteral("id")).toString();
            const bool alreadyKnown = !channelId.isEmpty()
                && storage.getChannelById(channelId);

            BackendChannel* channel = storage.addGroupChannel(doc.object());
            if (!channel) {
                return;
            }

            updateUserPreferences(BackendUserPreferences {
                QStringLiteral("group_channel_show"), channel->id,
                QStringLiteral("true")});

            if (!alreadyKnown) {
                emit storage.groupChannels.onNewChannel(*channel);
            }
            if (callback) {
                callback(*channel);
            }
        }));
}

void Backend::addUserToChannel (const BackendChannel& channel,
                                const QString& userID,
                                std::function<void()> callback)
{
	QJsonObject json {
		{"user_id", userID}
	};

	NetworkRequest request (_networkSession, "channels/" + channel.id + "/members");

	httpConnector.post(request, json, HttpResponseCallback(
        [callback = std::move(callback)](QVariant status, QByteArray) mutable {
            if (status.toInt() == QNetworkReply::NoError && callback) {
                callback();
            }
        }));
}

void Backend::removeUserFromChannel (const BackendChannel& channel, const QString& userID)
{
	NetworkRequest request (_networkSession, "channels/" + channel.id + "/members/" + userID);
	httpConnector.del (request);

	//there is no need of response callback, because a webSocket event 'user_removed' will come
}

void Backend::joinChannel (const BackendChannel& channel,
                           std::function<void()> callback)
{
	return addUserToChannel(channel, getLoginUser().id, std::move(callback));
}

void Backend::leaveChannel (const BackendChannel& channel)
{
	return removeUserFromChannel (channel, getLoginUser().id);
}

void Backend::addUserToTeam (const BackendTeam& team, const QString& userID)
{
	QJsonObject json {
		{"user_id", userID},
		{"team_id", team.id}
	};

	NetworkRequest request (_networkSession, "teams/" + team.id + "/members");

	httpConnector.post (request, json, HttpResponseCallback ([this](QVariant, QByteArray) {
#if 0
		QJsonDocument doc = QJsonDocument::fromJson(data);

		QString jsonString = doc.toJson(QJsonDocument::Indented);
		std::cout << jsonString.toStdString() << std::endl;
#endif

	}));
}

void Backend::removeUserFromTeam (const BackendTeam& team, const QString& userID)
{
	NetworkRequest request (_networkSession, "teams/" + team.id + "/members/" + userID);

	httpConnector.del (request);
}


void Backend::sendSubmitDialog (const QJsonDocument& json)
{
	// The dialog submission may contain private data; do not log the payload.
	NetworkRequest request (_networkSession, "actions/dialogs/submit");
	httpConnector.post (request, json, HttpResponseCallback ([] (QVariant, QByteArray) {
#if 0
		QJsonDocument doc2 = QJsonDocument::fromJson(data);
		QString jsonString = doc2.toJson(QJsonDocument::Indented);
		qDebug() << "SendSubmitDialog reply: " << jsonString.toStdString().c_str();
#endif
	}));
}

const BackendUser& Backend::getLoginUser () const
{
	return *storage.loginUser;
}

Storage& Backend::getStorage ()
{
	return storage;
}

ServerDialogsMap& Backend::getServerDialogsMap ()
{
	return serverDialogsMap;
}

} /* namespace Mattermost */
