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
#include <QJsonDocument>
#include <QJsonObject>
#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkCookie>
#include <QPointer>
#include <QStandardPaths>

#include "LocalPostDeleteTracker.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"
#include "Settings.h"
#include "UploadTrace.h"
#include "log.h"
#include "options/MLOptions.h"

namespace Mattermost {

QSet<HTTPConnector*> HTTPConnector::connectors;

static QNetworkDiskCache* createDiskCache ()
{
	QNetworkDiskCache* diskCache = new QNetworkDiskCache ();
	diskCache->setCacheDirectory (QStandardPaths::writableLocation(QStandardPaths::CacheLocation));

	const qint64 cacheSizeMB = std::max<qint64>(
		1,
		MLOptions::instance()
			->optionObject<int>(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT)
			->value().toLongLong());

	diskCache->setMaximumCacheSize (cacheSizeMB * 1024 * 1024);
	return diskCache;
}

static std::unique_ptr<QNetworkAccessManager> createNetworkManager ()
{
	auto manager = std::make_unique<QNetworkAccessManager>();
	manager->setCache(createDiskCache());
	manager->setAutoDeleteReplies(true);
	return manager;
}

HTTPConnector::HTTPConnector ()
:qnetworkManager (createNetworkManager ())
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
    LocalPostDeleteTracker::clear();

    // Destroy the old network manager while multipart request bodies are still
    // alive. Its replies may still reference their multipart body during
    // teardown.
	qnetworkManager = createNetworkManager();
    activeMultipartRequests.clear();
	processQueues();
}

void HTTPConnector::restartAllTransports ()
{
	// A network route/interface change affects every QNetworkAccessManager in
	// the process, not just the connector currently used by Backend. Work from a
	// snapshot because callbacks may indirectly modify the connector set.
	const QList<HTTPConnector*> snapshot = connectors.values();
	for (HTTPConnector* connector : snapshot) {
		if (connector) {
			connector->restartTransport();
		}
	}
}

void HTTPConnector::restartTransport ()
{
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
	const QList<QNetworkReply*> replies = activeReplies.values();
	for (QNetworkReply* reply : replies) {
		if (reply) {
			reply->abort();
		}
	}

	if (generation != restartGeneration) {
		// A callback performed a full reset while the old replies were aborted.
		restartingTransport = false;
		return;
	}

	qnetworkManager = createNetworkManager();
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
	});
}

void HTTPConnector::post(QNetworkRequest& request,
                         QSharedPointer<QHttpMultiPart> data,
                         HttpResponseCallback responseHandler)
{
    if (request.priority() != QNetworkRequest::LowPriority) {
        request.setPriority(QNetworkRequest::HighPriority);
    }

    qCInfo(lcUploadTrace).nospace()
        << "HTTP_UPLOAD_QUEUE url=" << request.url().toString()
        << " priority=" << static_cast<int>(request.priority())
        << " ua=" << request.rawHeader("User-Agent")
        << " cookie=" << (!request.rawHeader("Cookie").isEmpty() ? "yes" : "no")
        << " connectionId="
        << (request.rawHeader("Connection-Id").isEmpty()
                ? QByteArrayLiteral("none")
                : request.rawHeader("Connection-Id"));

    // Multipart framing remains endpoint-specific; common HTTP/TLS client
    // identity is applied centrally in enqueue() for every request method.
    enqueue(PendingRequest {
        Method::Post,
        request,
        QByteArray(),
        false,
        std::move(responseHandler),
        std::move(data),
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
	});
}

void HTTPConnector::del (QNetworkRequest& request)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}

    // Preserve the distinction the official client makes between a deletion
    // initiated here and the same post_deleted event received from another
    // client. Only exact DELETE /posts/{id} requests are recorded; unrelated
    // DELETE endpoints (thread unfollow, channel membership, etc.) are ignored.
    LocalPostDeleteTracker::noteRequest(request.url());

	enqueue(PendingRequest {
		Method::Delete,
		request,
		QByteArray(),
		false,
		HttpResponseCallback([](QVariant, QByteArray, const QNetworkReply&) {}),
        {},
	});
}

void HTTPConnector::enqueue(PendingRequest request)
{
    if (request.method == Method::Get) {
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

            qCInfo(lcUploadTrace).nospace()
                << "HTTP_UPLOAD_START reply=" << static_cast<const void*>(reply)
                << " url=" << reply->request().url().toString()
                << " contentType=" << reply->request().rawHeader("Content-Type")
                << " ua=" << reply->request().rawHeader("User-Agent")
                << " http2Allowed="
                << reply->request()
                       .attribute(QNetworkRequest::Http2AllowedAttribute, true)
                       .toBool();

            connect(reply,
                    &QNetworkReply::uploadProgress,
                    this,
                    [reply, lastPercent = -10](qint64 sent, qint64 total) mutable {
                        if (total <= 0) {
                            return;
                        }

                        const int percent =
                            static_cast<int>((sent * 100) / total);
                        if (percent < 100
                            && percent - lastPercent < 10) {
                            return;
                        }
                        lastPercent = percent;
                        qCInfo(lcUploadTrace).nospace()
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
		reply = qnetworkManager->deleteResource(request.request);
		break;
	}

	++activeRequests;
	activeReplies.insert(reply);
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
                NetworkRequest::updateSessionCookies(responseCookies);
            }

            if (currentReply->operation()
                    == QNetworkAccessManager::DeleteOperation
                && currentReply->error() != QNetworkReply::NoError) {
                LocalPostDeleteTracker::clearFailedRequest(
                    currentReply->request().url());
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
