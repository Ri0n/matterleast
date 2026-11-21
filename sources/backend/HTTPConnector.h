/**
 * @file HTTPConnector.h
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
 * the Free Software Foundation, either version 3 of the License, or
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

#include <functional>
#include <memory>

#include <QHash>
#include <QNetworkReply>
#include <QQueue>
#include <QSet>
#include <QPointer>
#include <QSharedPointer>

#include "backend/HttpResponseCallback.h"
#include "backend/types/BackendError.h"

class QHttpMultiPart;
class QNetworkAccessManager;

namespace Mattermost {

class QByteArrayCreator;
class NetworkSessionContext;
class LocalPostDeleteTracker;

class HTTPConnector: public QObject {
	Q_OBJECT
public:
    // Null context is an anonymous, credential-free HTTP transport without
    // shared disk caching. An explicit context must outlive every request.
    explicit HTTPConnector(NetworkSessionContext* session = nullptr,
                           LocalPostDeleteTracker* postDeleteTracker = nullptr);
	virtual ~HTTPConnector ();

    // Called before login requests; partitions the HTTP disk cache by
    // account. A missing/failed cache path disables disk caching, never
    // falls back to another account's cache.
    void configureAccountCache(const QString& accountIdentity);
	void reset ();
	void restartTransport ();
	static void restartAllTransports ();
    // Invalidate every HTTP connector belonging to exactly one session.
    // Do not abort requests on unrelated accounts or create an implicit
    // current-account singleton.
    static void resetSessionTransports(const NetworkSessionContext* session);

	void get (QNetworkRequest &request, HttpResponseCallback responseHandler);
	void post (QNetworkRequest &request, const QByteArrayCreator &data, HttpResponseCallback responseHandler);
	using UploadProgressHandler = std::function<void(qint64, qint64)>;

	void post(QNetworkRequest& request, QSharedPointer<QHttpMultiPart> data,
	          HttpResponseCallback responseHandler,
	          UploadProgressHandler uploadProgressHandler = {});
	void put (QNetworkRequest &request, const QByteArrayCreator &data, HttpResponseCallback responseHandler);
	void del (QNetworkRequest &request);
    void del(QNetworkRequest& request,
             const QByteArrayCreator& data,
             HttpResponseCallback responseHandler);

signals:
	void onNetworkError (uint32_t errorNumber, const QString& errorText);
	void onHttpError (uint32_t errorNumber, const QString& errorText);

private:
	enum class Method {
		Get,
		Post,
		Put,
		Delete,
	};

	struct PendingRequest {
		Method method = Method::Get;
		QNetworkRequest request;
		QByteArray data;
		bool jsonData = false;
		HttpResponseCallback responseHandler;
		QSharedPointer<QHttpMultiPart> multipartData;
		UploadProgressHandler uploadProgressHandler;
	};

	void enqueue (PendingRequest request);
	static void processQueues ();
	static HTTPConnector* connectorWithPendingRequest (bool lowPriority);
	void startRequest (PendingRequest request);
	virtual void setProcessReply (QNetworkReply* reply,
		std::function<void(QVariant,QByteArray,const QNetworkReply&)> responseHandler,
		quint64 requestGeneration);

	static QSet<HTTPConnector*> connectors;
    NetworkSessionContext* sessionContext = nullptr; // externally owned
    LocalPostDeleteTracker* postDeleteTracker = nullptr; // Storage-owned
    QString accountCacheDirectory;

	std::unique_ptr<QNetworkAccessManager> qnetworkManager;
	QQueue<PendingRequest> highPriorityRequests;
	QQueue<PendingRequest> lowPriorityRequests;
	QHash<QNetworkReply*, QPointer<QNetworkReply>> activeReplies;
	QHash<QNetworkReply*, PendingRequest> activeGetRequests;
	QHash<QNetworkReply*, QSharedPointer<QHttpMultiPart>> activeMultipartRequests;
	QSet<QNetworkReply*> replayedReplies;
	int activeRequests = 0;
	quint64 generation = 0;
	bool restartingTransport = false;
};

} /* namespace Mattermost */
