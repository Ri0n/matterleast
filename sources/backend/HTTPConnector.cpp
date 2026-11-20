/**
 * @file HTTPConnector.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Nov 29, 2021
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

#include "HTTPConnector.h"

#include <algorithm>
#include <QAbstractNetworkCache>
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QPointer>
#include <QStandardPaths>

#include "LocalPostDeleteTracker.h"
#include "NetworkRequest.h"
#include "NetworkSessionContext.h"
#include "NetworkSessionCookieJar.h"
#include "QByteArrayCreator.h"
#include "Settings.h"
#include "UploadTrace.h"
#include "log.h"
#include "options/MLOptions.h"

namespace Mattermost {

QSet<HTTPConnector*> HTTPConnector::connectors;

static QNetworkDiskCache* createDiskCache(const QString& directory)
{
    auto* diskCache = new QNetworkDiskCache();
    diskCache->setCacheDirectory(directory);

	const qint64 cacheSizeMB = std::max<qint64>(
		1,
		MLOptions::instance()
			->optionObject<int>(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT)
			->value().toLongLong());

	diskCache->setMaximumCacheSize (cacheSizeMB * 1024 * 1024);
	return diskCache;
}

static std::unique_ptr<QNetworkAccessManager> createNetworkManager(
    NetworkSessionContext* session, const QString& cacheDirectory)
{
    auto manager = std::make_unique<QNetworkAccessManager>();
    if (session)
        manager->setCookieJar(new NetworkSessionCookieJar(manager.get()));
    // Legacy single-server installations keep their previous cache path.
    // Authenticated multi-server contexts use an opaque account partition.
    const QString directory = session
        ? cacheDirectory
        : QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!directory.isEmpty())
        manager->setCache(createDiskCache(directory));
    manager->setAutoDeleteReplies(true);
    return manager;
}

HTTPConnector::HTTPConnector(NetworkSessionContext* session,
                             LocalPostDeleteTracker* deletionTracker)
    : sessionContext(session)
    , postDeleteTracker(deletionTracker)
    , qnetworkManager(session
        ? std::unique_ptr<QNetworkAccessManager>()
        : createNetworkManager(nullptr, {}))
{
	connectors.insert(this);

	auto* cacheSize = MLOptions::instance()->optionObject<int>(
		CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT);
	connect(cacheSize, &MLOptionObject::changed, this,
		[this](const QVariant& value) {
			if (!qnetworkManager) {
				return;
			}
			auto* diskCache =
				qobject_cast<QNetworkDiskCache*>(qnetworkManager->cache());
			if (!diskCache) {
				return;
			}
			const qint64 cacheSizeMB = std::max<qint64>(1, value.toLongLong());
			diskCache->setMaximumCacheSize(cacheSizeMB * 1024 * 1024);
		});
}

void HTTPConnector::configureAccountCache(const QString& accountIdentity)
{
    if (!sessionContext) return;
    const QString opaqueId = sessionContext->cachePartitionId(accountIdentity);
    const QString directory = opaqueId.isEmpty()
        ? QString()
        : QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/http-accounts/") + opaqueId;
    // A failing filesystem must disable the cache, not reuse a previous
    // account's directory (which could expose authenticated responses).
    const bool usable = !directory.isEmpty() && QDir().mkpath(directory);
    const QString next = usable ? directory : QString();
    if (accountCacheDirectory == next)
        return;
    accountCacheDirectory = next;
    if (!qnetworkManager) return;
    if (next.isEmpty()) {
        qnetworkManager->setCache(nullptr);
    } else if (auto* diskCache =
        qobject_cast<QNetworkDiskCache*>(qnetworkManager->cache())) {
        diskCache->setCacheDirectory(next);
    } else {
        qnetworkManager->setCache(createDiskCache(next));
    }
}

HTTPConnector::~HTTPConnector ()
{
	connectors.remove(this);
	activeRequests = 0;

    // QNetworkReply may still reference a multipart request body. Tear down the
    // manager before releasing those devices.
    qnetworkManager.reset();
    activeMultipartRequests.clear();

	processQueues();
}

void HTTPConnector::reset ()
{
	// Full application/session reset: discard queued work and suppress callbacks
	// from replies owned by the old application/storage generation.
	++generation;
	highPriorityRequests.clear ();
	lowPriorityRequests.clear ();
	activeReplies.clear();
	activeGetRequests.clear();
	replayedReplies.clear();
	activeRequests = 0;
	restartingTransport = false;
    if (postDeleteTracker)
        postDeleteTracker->clear();

    // Destroy the old network manager while multipart request bodies are still
    // alive. Its replies may still reference their multipart body during
    // teardown.
	qnetworkManager = sessionContext
        ? std::unique_ptr<QNetworkAccessManager>()
        : createNetworkManager(nullptr, accountCacheDirectory);
    activeMultipartRequests.clear();
	processQueues();
}

void HTTPConnector::restartAllTransports ()
{
	// A network route/interface change affects every QNetworkAccessManager in
	// the process, not just the connector currently used by Backend. Work from a
	// snapshot because callbacks may indirectly modify the connector set.
	QList<QPointer<HTTPConnector>> snapshot;
	for (HTTPConnector* connector : connectors) {
		snapshot.append(QPointer<HTTPConnector>(connector));
	}
	for (const QPointer<HTTPConnector>& connector : snapshot) {
		if (connector) {
			connector->restartTransport();
		}
	}
}

void HTTPConnector::restartTransport ()
{
    // Unused account services have no network manager to restart.
    if (!qnetworkManager && highPriorityRequests.isEmpty()
        && lowPriorityRequests.isEmpty())
        return;
	if (restartingTransport) {
		return;
	}

	restartingTransport = true;
	const quint64 restartGeneration = generation;
	LOG_DEBUG("Restarting HTTP transport after network route change; active="
	          << activeRequests << " queued="
	          << (highPriorityRequests.size() + lowPriorityRequests.size()));

	// GET is idempotent, so preserve the logical request and its callback. The
	// old reply is suppressed and the exact request is replayed on the fresh
	// QNetworkAccessManager. Mutating requests are deliberately not replayed: an
	// HTTP response may have been lost after the server committed the mutation.
	for (auto it = activeGetRequests.cbegin(); it != activeGetRequests.cend(); ++it) {
		replayedReplies.insert(it.key());
		const PendingRequest& request = it.value();
		if (request.request.priority() == QNetworkRequest::LowPriority) {
			lowPriorityRequests.prepend(request);
		} else {
			highPriorityRequests.prepend(request);
		}
	}

	// Abort old-route replies while their manager is still alive. Replayed GETs
	// skip their old callback; writes complete with OperationCanceledError so
	// their owner can apply its normal retry/uncertain-delivery policy.
	// abort() can synchronously invoke application callbacks and even enter
	// nested event loops (e.g. legacy dialogs). A previous abort may destroy
	// another reply or this connector before the next loop iteration.
	const QPointer<HTTPConnector> self(this);
	const QList<QPointer<QNetworkReply>> replies = activeReplies.values();
	for (const QPointer<QNetworkReply>& reply : replies) {
		if (!self) {
			return;
		}
		if (reply && activeReplies.contains(reply.data())) {
			reply->abort();
		}
	}
	if (!self) {
		return;
	}

	if (generation != restartGeneration) {
		// A callback performed a full reset while the old replies were aborted.
		restartingTransport = false;
		return;
	}

	qnetworkManager = sessionContext
        ? std::unique_ptr<QNetworkAccessManager>()
        : createNetworkManager(nullptr, accountCacheDirectory);
	restartingTransport = false;
	processQueues();
}

void HTTPConnector::get (QNetworkRequest& request, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}

	enqueue(PendingRequest {
		Method::Get,
		request,
		QByteArray(),
		false,
		std::move(responseHandler),
        {},
        {},
	});
}

void HTTPConnector::post (QNetworkRequest& request, const QByteArrayCreator& data, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}

	enqueue(PendingRequest {
		Method::Post,
		request,
		data,
		data.isJson(),
		std::move(responseHandler),
        {},
        {},
	});
}

void HTTPConnector::post(QNetworkRequest& request,
                         QSharedPointer<QHttpMultiPart> data,
                         HttpResponseCallback responseHandler,
                         UploadProgressHandler uploadProgressHandler)
{
    if (request.priority() != QNetworkRequest::LowPriority) {
        request.setPriority(QNetworkRequest::HighPriority);
    }

    qCDebug(lcUploadTrace).nospace()
        << "HTTP_UPLOAD_QUEUE priority=" << static_cast<int>(request.priority())
        << " hasCookie=" << !request.rawHeader("Cookie").isEmpty()
        << " hasConnectionId=" << !request.rawHeader("Connection-Id").isEmpty();

    // Multipart framing remains endpoint-specific; common HTTP/TLS client
    // identity is applied centrally in enqueue() for every request method.
    enqueue(PendingRequest {
        Method::Post,
        request,
        QByteArray(),
        false,
        std::move(responseHandler),
        std::move(data),
        std::move(uploadProgressHandler),
    });
}

void HTTPConnector::put (QNetworkRequest& request, const QByteArrayCreator& data, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}

	enqueue(PendingRequest {
		Method::Put,
		request,
		data,
		data.isJson(),
		std::move(responseHandler),
        {},
        {},
	});
}

void HTTPConnector::del (QNetworkRequest& request)
{
    del(request, QByteArrayCreator(QByteArray()),
        HttpResponseCallback([](QVariant, QByteArray, const QNetworkReply&) {}));
}

void HTTPConnector::del(QNetworkRequest& request,
                        const QByteArrayCreator& data,
                        HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}

    // Preserve the distinction the official client makes between a deletion
    // initiated here and the same post_deleted event received from another
    // client. Only exact DELETE /posts/{id} requests are recorded; unrelated
    // DELETE endpoints (thread unfollow, channel membership, etc.) are ignored.
    if (postDeleteTracker) {
        postDeleteTracker->noteRequest(request.url());
    }

	enqueue(PendingRequest {
		Method::Delete,
		request,
		data,
		data.isJson(),
		std::move(responseHandler),
        {},
        {},
	});
}

void HTTPConnector::enqueue(PendingRequest request)
{
    if (sessionContext) {
        // A request's caller may have used the legacy global constructor.
        // Never forward its authentication headers into another session.
        request.request.setRawHeader("Cookie", QByteArray());
        request.request.setRawHeader("X-CSRF-Token", QByteArray());
        request.request.setRawHeader("Authorization", QByteArray());
        if (request.method == Method::Get)
            sessionContext->applyDesktopProfile(request.request);
        else
            sessionContext->applyDesktopMutationProfile(request.request);
    } else if (request.method == Method::Get) {
        NetworkRequest::applyDesktopProfile(request.request);
    } else {
        NetworkRequest::applyDesktopMutationProfile(request.request);
    }

	if (request.request.priority() == QNetworkRequest::LowPriority) {
		lowPriorityRequests.enqueue(std::move(request));
	} else {
		highPriorityRequests.enqueue(std::move(request));
	}
	processQueues();
}

HTTPConnector* HTTPConnector::connectorWithPendingRequest(bool lowPriority)
{
	for (HTTPConnector* connector : connectors) {
		if (!connector || connector->restartingTransport) {
			continue;
		}
		const auto& queue = lowPriority
			? connector->lowPriorityRequests
			: connector->highPriorityRequests;
		if (!queue.isEmpty()) {
			return connector;
		}
	}
	return nullptr;
}

void HTTPConnector::processQueues()
{
    // QNetworkAccessManager already owns protocol-specific flow control:
    // HTTP/1.1 queues per host/port and HTTP/2 multiplexes according to the
    // negotiated connection and peer stream limits. A second process-global
    // gate only serializes unrelated work and hides available HTTP/2 capacity.
	while (true) {
		HTTPConnector* connector = connectorWithPendingRequest(false);
		bool lowPriority = false;
		if (!connector) {
			connector = connectorWithPendingRequest(true);
			lowPriority = true;
		}
		if (!connector) {
			return;
		}

		PendingRequest request = lowPriority
			? connector->lowPriorityRequests.dequeue()
			: connector->highPriorityRequests.dequeue();
		connector->startRequest(std::move(request));
	}
}

void HTTPConnector::startRequest(PendingRequest request)
{
    // Create isolated network resources only when this feature makes a request.
    if (!qnetworkManager)
        qnetworkManager = createNetworkManager(sessionContext, accountCacheDirectory);
	if (request.jsonData) {
		request.request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	}

	QNetworkReply* reply = nullptr;
	switch (request.method) {
	case Method::Get:
		reply = qnetworkManager->get(request.request);
		break;
	case Method::Post:
        if (request.multipartData) {
            reply = qnetworkManager->post(request.request, request.multipartData.data());

            qCDebug(lcUploadTrace).nospace()
                << "HTTP_UPLOAD_START reply=" << static_cast<const void*>(reply)
                << " http2Allowed="
                << reply->request()
                       .attribute(QNetworkRequest::Http2AllowedAttribute, true)
                       .toBool();

            connect(reply,
                    &QNetworkReply::uploadProgress,
                    this,
                    [reply,
                     progressHandler = std::move(request.uploadProgressHandler),
                     lastLoggedPercent = -10](
                        qint64 sent, qint64 total) mutable {
                        if (progressHandler) {
                            progressHandler(sent, total);
                        }

                        if (total <= 0) {
                            return;
                        }

                        const int percent =
                            static_cast<int>((sent * 100) / total);
                        if (percent < 100
                            && percent - lastLoggedPercent < 10) {
                            return;
                        }
                        lastLoggedPercent = percent;
                        qCDebug(lcUploadTrace).nospace()
                            << "HTTP_UPLOAD_PROGRESS reply="
                            << static_cast<const void*>(reply)
                            << " sent=" << sent
                            << " total=" << total
                            << " percent=" << percent;
                    });
        } else {
            reply = qnetworkManager->post(request.request, request.data);
        }
		break;
	case Method::Put:
		reply = qnetworkManager->put(request.request, request.data);
		break;
	case Method::Delete:
        if (request.data.isEmpty()) {
            reply = qnetworkManager->deleteResource(request.request);
        } else {
            reply = qnetworkManager->sendCustomRequest(
                request.request, QByteArrayLiteral("DELETE"), request.data);
        }
		break;
	}

	++activeRequests;
	activeReplies.insert(reply, QPointer<QNetworkReply>(reply));
	// Account for replies removed without finished(), including a manager
	// teardown inside a reentrant callback. Never retain a dangling raw key.
	connect(reply, &QObject::destroyed, this, [this, reply] {
		if (activeReplies.remove(reply)) {
			activeGetRequests.remove(reply);
			activeMultipartRequests.remove(reply);
			replayedReplies.remove(reply);
			if (activeRequests > 0) {
				--activeRequests;
			}
		}
	});
    if (request.multipartData) {
        activeMultipartRequests.insert(reply, request.multipartData);
    }
	if (request.method == Method::Get) {
		activeGetRequests.insert(reply, request);
	}
	setProcessReply(reply, std::move(request.responseHandler), generation);
}

void HTTPConnector::setProcessReply (QNetworkReply* reply,
		std::function<void (QVariant, QByteArray, const QNetworkReply&)> responseHandler,
		quint64 requestGeneration)
{
    QPointer<HTTPConnector> connectorGuard(this);
    QPointer<QNetworkReply> replyGuard(reply);

	connect(reply, &QNetworkReply::finished, this,
		[connectorGuard, replyGuard,
         responseHandler = std::move(responseHandler),
         requestGeneration]() mutable {
            HTTPConnector* connector = connectorGuard.data();
            QNetworkReply* currentReply = replyGuard.data();
            if (!connector || !currentReply) {
                return;
            }

			// reset() replaces the QNetworkAccessManager and releases the limiter
			// slots for its old replies. Those aborted replies may still emit
			// finished; never deliver them into callbacks belonging to the new
			// application/storage generation.
			if (requestGeneration != connector->generation) {
                connector->activeMultipartRequests.remove(currentReply);
				return;
			}

			const bool replayed =
                connector->replayedReplies.remove(currentReply);
			connector->activeReplies.remove(currentReply);
			connector->activeGetRequests.remove(currentReply);
            connector->activeMultipartRequests.remove(currentReply);
			if (replayed) {
				if (connector->activeRequests > 0) {
					--connector->activeRequests;
				}
				if (!connector->restartingTransport) {
					processQueues();
				}
				return;
			}

			const int statusCode = currentReply->error();
			auto data = currentReply->readAll();

            QList<QNetworkCookie> responseCookies;
            const auto rawHeaders = currentReply->rawHeaderPairs();
            for (const auto& header : rawHeaders) {
                if (header.first.compare(
                        QByteArrayLiteral("Set-Cookie"),
                        Qt::CaseInsensitive) != 0) {
                    continue;
                }
                responseCookies.append(
                    QNetworkCookie::parseCookies(header.second));
            }
            if (!responseCookies.isEmpty()) {
                if (connector->sessionContext) {
                    connector->sessionContext->updateSessionCookiesFrom(
                        currentReply->url(), responseCookies);
                } else {
                    NetworkRequest::updateSessionCookies(responseCookies);
                }
            }

            if (currentReply->operation()
                    == QNetworkAccessManager::DeleteOperation
                && currentReply->error() != QNetworkReply::NoError) {
                if (connector->postDeleteTracker) {
                    connector->postDeleteTracker->clearFailedRequest(
                        currentReply->request().url());
                }
            }

            // This callback is allowed to synchronously reset or even destroy
            // its UI owner together with this connector/QNetworkAccessManager.
            // The manager owns replies (setAutoDeleteReplies(true)), so after
            // invoking user code neither raw pointer is safe to touch until the
            // connector guard has been revalidated. In particular, do not call
            // deleteLater() on the reply here: reset/destruction may already
            // have deleted it.
			responseHandler(statusCode, qMove(data), *currentReply);

            connector = connectorGuard.data();
            if (!connector) {
                return;
            }

			// A response handler may also reset a still-live connector (for
			// example after an authentication failure). reset() has already
			// released all active slots, so do not decrement them twice.
			if (requestGeneration != connector->generation) {
				return;
			}

			if (connector->activeRequests > 0) {
				--connector->activeRequests;
			}
			if (!connector->restartingTransport) {
				processQueues();
			}
		});

#if QT_VERSION <= QT_VERSION_CHECK(5,15,0)
	connect(reply, qOverload<QNetworkReply::NetworkError>(&QNetworkReply::error),
#else
	connect(reply, qOverload<QNetworkReply::NetworkError>(&QNetworkReply::errorOccurred),
#endif
			this, [connectorGuard, replyGuard, requestGeneration](
                      QNetworkReply::NetworkError error) {
                HTTPConnector* connector = connectorGuard.data();
                QNetworkReply* currentReply = replyGuard.data();
                if (!connector || !currentReply
                    || requestGeneration != connector->generation
                    || connector->replayedReplies.contains(currentReply)) {
                    return;
                }
				emit connector->onNetworkError(
                    error, currentReply->errorString());
			});
}

} /* namespace Mattermost */
