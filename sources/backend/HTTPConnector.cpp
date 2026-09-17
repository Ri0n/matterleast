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

#include <QAbstractNetworkCache>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>

#include "LocalPostDeleteTracker.h"
#include "QByteArrayCreator.h"
#include "Settings.h"
#include "log.h"

namespace Mattermost {

QSet<HTTPConnector*> HTTPConnector::connectors;
int HTTPConnector::globalActiveRequests = 0;

static QNetworkDiskCache* createDiskCache ()
{
	QNetworkDiskCache* diskCache = new QNetworkDiskCache ();
	diskCache->setCacheDirectory (QStandardPaths::writableLocation(QStandardPaths::CacheLocation));

	QSettings settings;
	unsigned cacheSizeMB = settings.value(CACHE_SIZE_MB, CACHE_SIZE_MB_DEFAULT).toUInt();

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
}

HTTPConnector::~HTTPConnector ()
{
	connectors.remove(this);
	globalActiveRequests = qMax(0, globalActiveRequests - activeRequests);
	activeRequests = 0;
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
	globalActiveRequests = qMax(0, globalActiveRequests - activeRequests);
	activeRequests = 0;
	restartingTransport = false;
    LocalPostDeleteTracker::clear();

	qnetworkManager = createNetworkManager();
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
	});
}

void HTTPConnector::post (QNetworkRequest& request, const QByteArrayCreator& data, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

	enqueue(PendingRequest {
		Method::Post,
		request,
		data,
		data.isJson(),
		std::move(responseHandler),
	});
}

void HTTPConnector::put (QNetworkRequest& request, const QByteArrayCreator& data, HttpResponseCallback responseHandler)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

	enqueue(PendingRequest {
		Method::Put,
		request,
		data,
		data.isJson(),
		std::move(responseHandler),
	});
}

void HTTPConnector::del (QNetworkRequest& request)
{
	if (request.priority() != QNetworkRequest::LowPriority) {
		request.setPriority(QNetworkRequest::HighPriority);
	}
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

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
	});
}

void HTTPConnector::enqueue(PendingRequest request)
{
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
		if (!connector) {
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
	while (globalActiveRequests < MaxConcurrentRequests) {
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
		reply = qnetworkManager->post(request.request, request.data);
		break;
	case Method::Put:
		reply = qnetworkManager->put(request.request, request.data);
		break;
	case Method::Delete:
		reply = qnetworkManager->deleteResource(request.request);
		break;
	}

	++activeRequests;
	++globalActiveRequests;
	activeReplies.insert(reply);
	if (request.method == Method::Get) {
		activeGetRequests.insert(reply, request);
	}
	setProcessReply(reply, std::move(request.responseHandler), generation);
}

void HTTPConnector::setProcessReply (QNetworkReply* reply,
		std::function<void (QVariant, QByteArray, const QNetworkReply&)> responseHandler,
		quint64 requestGeneration)
{
	connect(reply, &QNetworkReply::finished, this,
		[this, reply, responseHandler = std::move(responseHandler), requestGeneration]() mutable {
			// reset() replaces the QNetworkAccessManager and releases the limiter
			// slots for its old replies. Those aborted replies may still emit
			// finished; never deliver them into callbacks belonging to the new
			// application/storage generation.
			if (requestGeneration != generation) {
				reply->deleteLater();
				return;
			}

			const bool replayed = replayedReplies.remove(reply);
			activeReplies.remove(reply);
			activeGetRequests.remove(reply);
			if (replayed) {
				reply->deleteLater();
				if (activeRequests > 0) {
					--activeRequests;
				}
				if (globalActiveRequests > 0) {
					--globalActiveRequests;
				}
				if (!restartingTransport) {
					processQueues();
				}
				return;
			}

			const int statusCode = reply->error();
			auto data = reply->readAll();

            if (reply->operation() == QNetworkAccessManager::DeleteOperation
                && reply->error() != QNetworkReply::NoError) {
                LocalPostDeleteTracker::clearFailedRequest(reply->request().url());
            }

			responseHandler(statusCode, qMove(data), *reply);
			reply->deleteLater();

			// A response handler is allowed to reset the connector itself (for
			// example after an authentication failure). In that case reset() has
			// already released all active slots, so do not decrement them twice.
			if (requestGeneration != generation) {
				return;
			}

			if (activeRequests > 0) {
				--activeRequests;
			}
			if (globalActiveRequests > 0) {
				--globalActiveRequests;
			}
			if (!restartingTransport) {
				processQueues();
			}
		});

#if QT_VERSION <= QT_VERSION_CHECK(5,15,0)
	connect(reply, qOverload<QNetworkReply::NetworkError>(&QNetworkReply::error),
#else
	connect(reply, qOverload<QNetworkReply::NetworkError>(&QNetworkReply::errorOccurred),
#endif
			this, [this, reply, requestGeneration](QNetworkReply::NetworkError error) {
		if (requestGeneration != generation || replayedReplies.contains(reply)) {
			return;
		}
		emit onNetworkError (error, reply->errorString());
	});
}

} /* namespace Mattermost */
