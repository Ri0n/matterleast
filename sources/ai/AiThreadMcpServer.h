#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QPair>

class QLocalSocket;
namespace Mattermost {
class Backend;
class BackendPost;

class AiThreadMcpServer final : public QObject
{
    Q_OBJECT
public:
    static AiThreadMcpServer& instance(Backend& backend);
    QString createSession(const QString& channelId, const QString& rootId);
    void revokeSession(const QString& token);
    void setWriteEnabled(const QString& token, bool enabled);
    QString endpoint() const { return _endpoint; }
    bool listening() const { return _server.isListening(); }

private:
    explicit AiThreadMcpServer(Backend& backend);
    struct Cursor {
        QString postId;
        quint64 createAt = 0;
    };
    struct Session {
        QString channelId;
        QString rootId;
        bool writeEnabled = false;
        QHash<QString, Cursor> cursors;
    };

    void processLine(QLocalSocket* socket, const QByteArray& bytes);
    void sendResponse(QLocalSocket* socket, const QJsonValue& id,
                      const QJsonObject& result);
    void sendToolResult(QLocalSocket* socket, const QJsonValue& id,
                        const QJsonObject& value, bool error = false);
    void sendError(QLocalSocket* socket, const QJsonValue& id,
                   int code, const QString& reason);
    void readThread(QLocalSocket* socket, const QJsonValue& id,
                    const QString& token, const QJsonObject& arguments);
    void readPost(QLocalSocket* socket, const QJsonValue& id,
                  const QString& token, const QJsonObject& arguments);
    void postReply(QLocalSocket* socket, const QJsonValue& id,
                   const QString& token, const QJsonObject& arguments,
                   bool quoteRequired);
    QJsonObject serializePost(const BackendPost& post) const;

    Backend& _backend;
    QLocalServer _server;
    QString _endpoint;
    QHash<QString, Session> _sessions;
    QHash<QLocalSocket*, QByteArray> _buffers;
};
} // namespace Mattermost
