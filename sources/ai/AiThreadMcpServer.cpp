#include "AiThreadMcpServer.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QUuid>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/PostCreateService.h"
#include "backend/PostProps.h"
#include "backend/PostRepository.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "chat-area/QuotedReplyFormat.h"

namespace Mattermost {
namespace {
QString newToken()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject toolSpec(const QString& name, const QString& description,
                     const QJsonObject& properties,
                     const QJsonArray& required, bool readonly)
{
    QJsonObject schema {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("required"), required}
    };
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), description},
        {QStringLiteral("inputSchema"), schema},
        {QStringLiteral("annotations"), QJsonObject {
             {QStringLiteral("readOnlyHint"), readonly}
         }}
    };
}

QJsonObject stringProperty(const QString& description)
{
    return {{QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("description"), description}};
}

QJsonObject toolCatalog()
{
    const QJsonArray tools {
        toolSpec(QStringLiteral("get_thread"),
                 QStringLiteral("Read a chronologically paginated Mattermost thread including its root. "
                                "Use next_cursor until has_more=false for a complete snapshot."),
                 {{QStringLiteral("cursor"), stringProperty(QStringLiteral("Opaque next_cursor from the previous page; omit for first page"))},
                  {QStringLiteral("limit"), QJsonObject {{QStringLiteral("type"), QStringLiteral("integer")},
                                                        {QStringLiteral("minimum"), 1},
                                                        {QStringLiteral("maximum"), 100}}}},
                 {}, true),
        toolSpec(QStringLiteral("get_post"),
                 QStringLiteral("Read a single post belonging to the current thread."),
                 {{QStringLiteral("post_id"), stringProperty(QStringLiteral("Mattermost post ID"))}},
                 {QStringLiteral("post_id")}, true),
        toolSpec(QStringLiteral("post_reply"),
                 QStringLiteral("Publish a new reply in the current Mattermost thread as the signed-in user. "
                                "Requires that the user explicitly enables write access in the AI chat."),
                 {{QStringLiteral("message"), stringProperty(QStringLiteral("Markdown message to publish"))}},
                 {QStringLiteral("message")}, false),
        toolSpec(QStringLiteral("reply_to_post"),
                 QStringLiteral("Publish a quoted response to a specified post within this thread, "
                                "using MatterLeast's existing quote metadata and fallback format. "
                                "Requires write access."),
                 {{QStringLiteral("post_id"), stringProperty(QStringLiteral("Target post ID inside this thread"))},
                  {QStringLiteral("message"), stringProperty(QStringLiteral("Markdown reply"))}},
                 {QStringLiteral("post_id"), QStringLiteral("message")}, false)
    };
    return {{QStringLiteral("tools"), tools}};
}
}

AiThreadMcpServer& AiThreadMcpServer::instance(Backend& backend)
{
    static QPointer<AiThreadMcpServer> instance;
    if (!instance)
        instance = new AiThreadMcpServer(backend);
    return *instance;
}

AiThreadMcpServer::AiThreadMcpServer(Backend& backend)
    : QObject(&backend), _backend(backend)
{
    _endpoint = QStringLiteral("matterleast-ai-") + newToken();
    // The endpoint and credentials are ephemeral; no listening TCP port.
    _server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!_server.listen(_endpoint))
        qWarning("MatterLeast AI MCP: cannot create local socket");

    connect(&_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = _server.nextPendingConnection()) {
            _buffers.insert(socket, {});
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                QByteArray& buffer = _buffers[socket];
                buffer += socket->readAll();
                if (buffer.size() > 1024 * 1024) {
                    socket->disconnectFromServer();
                    return;
                }
                while (true) {
                    const int end = buffer.indexOf('\n');
                    if (end < 0)
                        break;
                    const QByteArray line = buffer.left(end);
                    buffer.remove(0, end + 1);
                    processLine(socket, line);
                }
            });
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                _buffers.remove(socket);
                socket->deleteLater();
            });
        }
    });
}

QString AiThreadMcpServer::createSession(const QString& channelId, const QString& rootId)
{
    if (!_server.isListening() || !_backend.loggedIn()
        || channelId.isEmpty() || rootId.isEmpty()
        || !_backend.getStorage().getChannelById(channelId)) {
        return {};
    }
    const QString token = newToken();
    _sessions.insert(token, Session {channelId, rootId,
        _backend.getLoginUser().id, NetworkRequest::host(), false, {}});
    return token;
}

bool AiThreadMcpServer::authorized(const QString& token) const
{
    const auto it = _sessions.constFind(token);
    return _backend.loggedIn() && it != _sessions.cend()
        && it->loginUserId == _backend.getLoginUser().id
        && it->serverHost == NetworkRequest::host();
}

void AiThreadMcpServer::revokeSession(const QString& token)
{
    _sessions.remove(token);
}

void AiThreadMcpServer::setWriteEnabled(const QString& token, bool enabled)
{
    auto it = _sessions.find(token);
    if (it != _sessions.end())
        it->writeEnabled = enabled;
}

void AiThreadMcpServer::sendResponse(QLocalSocket* socket, const QJsonValue& id,
                                     const QJsonObject& result)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;
    QJsonObject object {{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("result"), result}};
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
}

void AiThreadMcpServer::sendToolResult(QLocalSocket* socket, const QJsonValue& id,
                                      const QJsonObject& value, bool error)
{
    QJsonObject result {
        {QStringLiteral("content"), QJsonArray {QJsonObject {
             {QStringLiteral("type"), QStringLiteral("text")},
             {QStringLiteral("text"), QString::fromUtf8(
                 QJsonDocument(value).toJson(QJsonDocument::Compact))}
         }}}
    };
    if (error)
        result.insert(QStringLiteral("isError"), true);
    sendResponse(socket, id, result);
}

void AiThreadMcpServer::sendError(QLocalSocket* socket, const QJsonValue& id,
                                  int code, const QString& reason)
{
    if (!socket || socket->state() != QLocalSocket::ConnectedState)
        return;
    QJsonObject response {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject {
             {QStringLiteral("code"), code},
             {QStringLiteral("message"), reason}
         }}
    };
    socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
}

QJsonObject AiThreadMcpServer::serializePost(const BackendPost& post) const
{
    QJsonArray attachments;
    for (const auto& file : post.files) {
        attachments.append(QJsonObject {
            {QStringLiteral("id"), file.id},
            {QStringLiteral("name"), file.name},
            {QStringLiteral("mime_type"), file.mimeType},
            {QStringLiteral("size"),
             QJsonValue::fromVariant(QVariant::fromValue<qlonglong>(file.size))}
        });
    }
    return {
        {QStringLiteral("id"), post.id},
        {QStringLiteral("attachments"), attachments},
        {QStringLiteral("deleted"), post.isDeleted || post.delete_at != 0},
        {QStringLiteral("root_id"), post.root_id},
        {QStringLiteral("author_id"), post.user_id},
        {QStringLiteral("author"), post.getDisplayAuthorName()},
        {QStringLiteral("message"), post.message},
        {QStringLiteral("created_at"),
         QJsonValue::fromVariant(QVariant::fromValue<qlonglong>(post.create_at))},
        {QStringLiteral("edited_at"),
         QJsonValue::fromVariant(QVariant::fromValue<qlonglong>(post.edit_at))},
        {QStringLiteral("reply_to_post_id"),
         post.props.toObject().value(QString::fromLatin1(PostProps::ReplyToPostId))}
    };
}

void AiThreadMcpServer::processLine(QLocalSocket* socket, const QByteArray& bytes)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return;

    const QJsonObject wrapped = document.object();
    const QJsonObject rpc = wrapped.value(QStringLiteral("mcp")).toObject();
    const QJsonValue id = rpc.value(QStringLiteral("id"));
    const QString token = wrapped.value(QStringLiteral("token")).toString();
    if (!authorized(token)) {
        if (!id.isUndefined())
            sendError(socket, id, -32001, QStringLiteral("MCP session expired or unauthorized"));
        return;
    }

    const QString method = rpc.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("notifications/initialized")
        || method == QStringLiteral("notifications/cancelled"))
        return;
    if (method == QStringLiteral("initialize")) {
        // Negotiate the MCP version this implementation actually supports,
        // rather than claiming every arbitrary client version is implemented.
        sendResponse(socket, id, {
            {QStringLiteral("protocolVersion"), QStringLiteral("2025-03-26")},
            {QStringLiteral("capabilities"), QJsonObject {
                 {QStringLiteral("tools"), QJsonObject {}}
             }},
            {QStringLiteral("serverInfo"), QJsonObject {
                 {QStringLiteral("name"), QStringLiteral("matterleast")},
                 {QStringLiteral("version"), QStringLiteral("0.1.0")}
             }}
        });
    } else if (method == QStringLiteral("ping")) {
        sendResponse(socket, id, {});
    } else if (method == QStringLiteral("tools/list")) {
        sendResponse(socket, id, toolCatalog());
    } else if (method == QStringLiteral("tools/call")) {
        const QJsonObject params = rpc.value(QStringLiteral("params")).toObject();
        const QString name = params.value(QStringLiteral("name")).toString();
        const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();
        if (name == QStringLiteral("get_thread"))
            readThread(socket, id, token, args);
        else if (name == QStringLiteral("get_post"))
            readPost(socket, id, token, args);
        else if (name == QStringLiteral("post_reply"))
            postReply(socket, id, token, args, false);
        else if (name == QStringLiteral("reply_to_post"))
            postReply(socket, id, token, args, true);
        else
            sendError(socket, id, -32601, QStringLiteral("Unknown tool"));
    } else if (!id.isUndefined()) {
        sendError(socket, id, -32601, QStringLiteral("Method not supported"));
    }
}

void AiThreadMcpServer::readThread(QLocalSocket* socket, const QJsonValue& id,
                                   const QString& token, const QJsonObject& args)
{
    auto session = _sessions.find(token);
    if (session == _sessions.end())
        return;
    BackendChannel* channel = _backend.getStorage().getChannelById(session->channelId);
    if (!channel) {
        sendToolResult(socket, id, {{QStringLiteral("error"), QStringLiteral("Channel no longer available")}}, true);
        return;
    }
    const QString cursorId = args.value(QStringLiteral("cursor")).toString();
    Cursor cursor;
    if (!cursorId.isEmpty()) {
        if (!session->cursors.contains(cursorId)) {
            sendToolResult(socket, id, {{QStringLiteral("error"), QStringLiteral("Unknown thread cursor")}}, true);
            return;
        }
        cursor = session->cursors.value(cursorId);
    }
    const int limit = qBound(1, args.value(QStringLiteral("limit")).toInt(50), 100);
    const QString root = session->rootId;
    const QString channelId = session->channelId;
    QPointer<QLocalSocket> peer(socket);
    PostRepository::instance(_backend).loadThreadPage(
        *channel, root, limit, cursor.postId, cursor.createAt,
        [this, peer, token, id, root, channelId](const PostRepository::Page& page) {
        if (!peer || !authorized(token)
            || !_backend.loggedIn()) return;
        BackendChannel* valid = _backend.getStorage().getChannelById(channelId);
        if (!page.success || !valid) {
            sendToolResult(peer, id, {{QStringLiteral("error"), QStringLiteral("Thread retrieval failed")}}, true);
            return;
        }
        QJsonArray posts;
        QString lastId;
        quint64 lastCreated = 0;
        for (const QString& postId : page.postIds) {
            const BackendPost* post = valid->postIdToPost.value(postId, nullptr);
            if (!post || (post->id != root && post->root_id != root))
                continue;
            posts.append(serializePost(*post));
            lastId = postId;
            lastCreated = page.createAtById.value(postId, post->create_at);
        }
        QJsonObject result {
            {QStringLiteral("root_id"), root},
            {QStringLiteral("posts"), posts},
            {QStringLiteral("has_more"), page.hasNext || !page.nextPostId.isEmpty()}
        };
        if (!lastId.isEmpty() && result.value(QStringLiteral("has_more")).toBool()) {
            const QString next = newToken();
            _sessions[token].cursors.insert(next, {lastId, lastCreated});
            result.insert(QStringLiteral("next_cursor"), next);
        }
        sendToolResult(peer, id, result);
    });
}

void AiThreadMcpServer::readPost(QLocalSocket* socket, const QJsonValue& id,
                                 const QString& token, const QJsonObject& args)
{
    const auto session = _sessions.value(token);
    const QString postId = args.value(QStringLiteral("post_id")).toString();
    if (postId.isEmpty()) {
        sendToolResult(socket, id, {{QStringLiteral("error"), QStringLiteral("post_id required")}}, true);
        return;
    }
    QPointer<QLocalSocket> peer(socket);
    PostRepository::instance(_backend).loadPost(
        postId, [this, peer, id, token, session](const PostRepository::PostResult& result) {
        if (!peer || !authorized(token)) return;
        BackendChannel* channel = _backend.getStorage().getChannelById(session.channelId);
        BackendPost* post = channel ? channel->postIdToPost.value(result.postId, nullptr) : nullptr;
        if (!result.success || !post ||
            (post->id != session.rootId && post->root_id != session.rootId)) {
            sendToolResult(peer, id, {{QStringLiteral("error"), QStringLiteral("Post not found in this thread")}}, true);
            return;
        }
        sendToolResult(peer, id, {{QStringLiteral("post"), serializePost(*post)}});
    });
}

void AiThreadMcpServer::postReply(QLocalSocket* socket, const QJsonValue& id,
                                  const QString& token, const QJsonObject& args,
                                  bool quoteRequired)
{
    auto session = _sessions.find(token);
    if (session == _sessions.end())
        return;
    const QString message = args.value(QStringLiteral("message")).toString().trimmed();
    const QString quoteId = args.value(QStringLiteral("post_id")).toString();
    if (!session->writeEnabled || message.isEmpty() || message.size() > 16000
        || (quoteRequired && quoteId.isEmpty())) {
        sendToolResult(socket, id, {{QStringLiteral("error"),
            QStringLiteral("Writing is disabled, message invalid, or exceeds Mattermost's single-post limit")}}, true);
        return;
    }
    BackendChannel* channel = _backend.getStorage().getChannelById(session->channelId);
    if (!channel) {
        sendToolResult(socket, id, {{QStringLiteral("error"), QStringLiteral("Channel no longer available")}}, true);
        return;
    }
    const QString rootId = session->rootId;
    const QString channelId = session->channelId;
    QPointer<QLocalSocket> peer(socket);
    const auto create = [this, peer, id, token, channelId, rootId, message](
                            const BackendPost* quoted) {
        if (!peer || !authorized(token) || !_sessions[token].writeEnabled)
            return;
        BackendChannel* target = _backend.getStorage().getChannelById(channelId);
        if (!target) return;
        QJsonObject props;
        QString wireMessage = message;
        if (quoted) {
            props.insert(QString::fromLatin1(PostProps::ReplyToPostId), quoted->id);
            wireMessage.prepend(QuotedReplyFormat::buildFallback(
                quoted->id, quoted->getDisplayAuthorName(), quoted->message,
                !quoted->files.empty()));
        }
        // A server-side acknowledgement is required. Never return success when
        // the request was only enqueued or failed after transmission.
        // Match the existing composer pending_post_id convention. This keeps
        // Mattermost's create deduplication behavior consistent.
        const QString pending = _backend.getLoginUser().id + QLatin1Char(':')
            + QString::number(QDateTime::currentMSecsSinceEpoch());
        PostCreateService::instance(_backend).createPostDetailed(
            *target, wireMessage, {}, rootId, props, pending,
            [this, peer, id, token](PostCreateService::CreatePostResult result) {
                if (!peer || !authorized(token)) return;
                if (!result.success()) {
                    sendToolResult(peer, id, {
                        {QStringLiteral("error"),
                         result.errorText.isEmpty()
                            ? QStringLiteral("Post not confirmed; check the thread before retrying")
                            : result.errorText},
                        {QStringLiteral("outcome_uncertain"), result.retryable()}
                    }, true);
                } else {
                    sendToolResult(peer, id, {
                        {QStringLiteral("posted"), true},
                        {QStringLiteral("post_id"), result.post->id}
                    });
                }
            });
    };
    if (!quoteRequired) {
        create(nullptr);
        return;
    }
    // Only targets in this thread can be quoted; do not allow a model to quote
    // an unrelated channel even when its id is valid on this account.
    PostRepository::instance(_backend).loadPost(
        quoteId, [this, peer, token, id, rootId, channelId, create](
                     const PostRepository::PostResult& result) {
        if (!peer || !authorized(token)) return;
        BackendChannel* channel = _backend.getStorage().getChannelById(channelId);
        BackendPost* quoted = channel
            ? channel->postIdToPost.value(result.postId, nullptr) : nullptr;
        if (!result.success || !quoted
            || (quoted->id != rootId && quoted->root_id != rootId)) {
            sendToolResult(peer, id, {
                {QStringLiteral("error"), QStringLiteral("Quoted post is not in current thread")}
            }, true);
            return;
        }
        create(quoted);
    });
}
} // namespace Mattermost
