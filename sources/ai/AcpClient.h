#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace Mattermost {

// ACP v1 JSON-RPC client over a persistent stdio subprocess.
// The agent owns model credentials and its conversation history. The client
// advertises no filesystem/terminal capabilities and delegates tools via MCP.
class AcpClient final : public QObject
{
    Q_OBJECT
public:
    struct Configuration {
        QString program;
        QStringList arguments;
        QString workingDirectory;
        QString mcpProgram;
        QString mcpEndpoint;
        QString mcpToken;
    };

    explicit AcpClient(QObject* parent = nullptr);
    ~AcpClient() override;

    void start(const Configuration& configuration, const QString& restoreSessionId = {});
    bool prompt(const QString& text);
    void cancel();
    void stop();
    void answerPermission(const QJsonValue& requestId, const QString& optionId);

    bool ready() const { return _ready; }
    bool busy() const { return _busy; }
    QString sessionId() const { return _sessionId; }

signals:
    void readyChanged(bool ready);
    void statusChanged(const QString& message);
    void messageChunk(const QString& text);
    void thoughtChunk(const QString& text);
    void toolChanged(const QString& title, const QString& status);
    void permissionRequested(const QJsonValue& requestId, const QJsonObject& params);
    void turnFinished(const QString& stopReason);
    void failed(const QString& message);

private:
    enum class Request { Initialize, NewSession, ResumeSession, LoadSession, Prompt };
    qint64 request(const QString& method, const QJsonObject& params, Request type);
    void send(const QJsonObject& message);
    void notify(const QString& method, const QJsonObject& params);
    void startSession();
    QJsonObject sessionParams(bool includeId) const;
    void sendQueuedPrompt();
    void onStdout();
    void handle(const QJsonObject& message);
    void handleResult(Request kind, const QJsonObject& result);
    void handleUpdate(const QJsonObject& update);
    void cancelPermissions();
    void fail(const QString& message);
    static QString permissionKey(const QJsonValue& id);

    QProcess _process;
    QTimer _phaseTimeout;
    Configuration _configuration;
    QByteArray _stdout;
    QHash<qint64, Request> _requests;
    QHash<QString, QJsonValue> _permissions;
    qint64 _nextId = 1;
    QString _sessionId;
    QString _restoreSessionId;
    QString _queuedPrompt;
    bool _canResume = false;
    bool _canLoad = false;
    bool _canClose = false;
    bool _ready = false;
    bool _busy = false;
    bool _cancelled = false;
    bool _stopping = false;
    bool _failed = false;
};

} // namespace Mattermost
