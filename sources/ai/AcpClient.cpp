#include "AcpClient.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <utility>

namespace Mattermost {
namespace {
constexpr int MaxMessageBytes = 8 * 1024 * 1024;

QJsonObject textBlock(const QString& text)
{
    return {{QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), text}};
}

QJsonObject cancelledOutcome()
{
    return {{QStringLiteral("outcome"), QJsonObject{
        {QStringLiteral("outcome"), QStringLiteral("cancelled")}
    }}};
}
} // namespace

AcpClient::AcpClient(QObject* parent) : QObject(parent), _process(this)
{
    _phaseTimeout.setSingleShot(true);
    connect(&_phaseTimeout, &QTimer::timeout, this, [this] {
        fail(tr("ACP agent did not respond within the timeout."));
    });

    connect(&_process, &QProcess::started, this, [this] {
        emit statusChanged(tr("Initializing ACP agent…"));
        request(QStringLiteral("initialize"), QJsonObject {
            {QStringLiteral("protocolVersion"), 1},
            // We do not grant filesystem or terminal access on behalf of the
            // user. The agent can instead call the thread-scoped MCP tools.
            {QStringLiteral("clientCapabilities"), QJsonObject{}},
            {QStringLiteral("clientInfo"), QJsonObject {
                {QStringLiteral("name"), QStringLiteral("matterleast")},
                {QStringLiteral("title"), QStringLiteral("MatterLeast")},
                {QStringLiteral("version"), QStringLiteral("2.1")}
            }}
        }, Request::Initialize);
        _phaseTimeout.start(30000);
    });
    connect(&_process, &QProcess::readyReadStandardOutput,
            this, &AcpClient::onStdout);
    connect(&_process, &QProcess::readyReadStandardError, this, [this] {
        // stderr is for the agent's own diagnostics. Never include it in
        // protocol parsing or logs that could expose MCP credentials.
        _process.readAllStandardError();
    });
    connect(&_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (!_stopping && error == QProcess::FailedToStart)
            fail(tr("Cannot start the configured ACP agent executable."));
    });
    connect(&_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
        onStdout();
        if (!_stopping && !_failed)
            fail(tr("ACP agent exited unexpectedly (code %1).").arg(code));
    });
}

AcpClient::~AcpClient()
{
    stop();
}

void AcpClient::start(const Configuration& configuration,
                      const QString& restoreSessionId)
{
    stop();
    _configuration = configuration;
    _restoreSessionId = restoreSessionId;
    _sessionId.clear();
    _stdout.clear();
    _requests.clear();
    _permissions.clear();
    _toolTitles.clear();
    ++_cancelEpoch;
    _queuedPrompt.clear();
    _nextId = 1;
    _canResume = false;
    _canLoad = false;
    _canClose = false;
    _ready = false;
    _busy = false;
    _cancelled = false;
    _failed = false;
    _stopping = false;

    if (_configuration.program.isEmpty() ||
        !QDir::isAbsolutePath(_configuration.workingDirectory) ||
        !QDir::isAbsolutePath(_configuration.mcpProgram) ||
        _configuration.mcpEndpoint.isEmpty() ||
        _configuration.mcpToken.isEmpty()) {
        fail(tr("Invalid ACP agent or MCP session configuration."));
        return;
    }

    _process.setProgram(_configuration.program);
    _process.setArguments(_configuration.arguments);
    _process.setWorkingDirectory(_configuration.workingDirectory);
    _process.setProcessChannelMode(QProcess::SeparateChannels);
    _process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    emit statusChanged(tr("Starting ACP agent…"));
    _process.start();
}

bool AcpClient::prompt(const QString& text)
{
    if (_failed || _stopping || _busy || text.trimmed().isEmpty())
        return false;
    if (_process.state() == QProcess::NotRunning)
        return false;
    _busy = true;
    _cancelled = false;
    _queuedPrompt = text;
    if (_ready)
        sendQueuedPrompt();
    return true;
}

qint64 AcpClient::request(const QString& method, const QJsonObject& params,
                          Request kind)
{
    const qint64 id = _nextId++;
    _requests.insert(id, kind);
    send(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), double(id)},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params}
    });
    return id;
}

void AcpClient::send(const QJsonObject& message)
{
    if (_process.state() != QProcess::Running)
        return;
    const QByteArray wire =
        QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (_process.write(wire) < 0)
        fail(tr("ACP transport write failed."));
}

void AcpClient::notify(const QString& method, const QJsonObject& params)
{
    send(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params}
    });
}

QJsonObject AcpClient::sessionParams(bool includeId) const
{
    const QJsonArray env {
        QJsonObject{{QStringLiteral("name"), QStringLiteral("MATTERLEAST_MCP_SOCKET")},
                    {QStringLiteral("value"), _configuration.mcpEndpoint}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("MATTERLEAST_MCP_TOKEN")},
                    {QStringLiteral("value"), _configuration.mcpToken}}
    };
    const QJsonObject mcpServer {
        {QStringLiteral("name"), QStringLiteral("matterleast")},
        {QStringLiteral("command"), _configuration.mcpProgram},
        {QStringLiteral("args"), QJsonArray{}},
        {QStringLiteral("env"), env}
    };
    QJsonObject params {
        {QStringLiteral("cwd"), _configuration.workingDirectory},
        {QStringLiteral("mcpServers"), QJsonArray{mcpServer}}
    };
    if (includeId)
        params.insert(QStringLiteral("sessionId"), _restoreSessionId);
    return params;
}

void AcpClient::startSession()
{
    emit statusChanged(tr("Creating ACP session and connecting MCP tools…"));
    if (!_restoreSessionId.isEmpty() && _canResume) {
        request(QStringLiteral("session/resume"),
                sessionParams(true), Request::ResumeSession);
    } else if (!_restoreSessionId.isEmpty() && _canLoad) {
        request(QStringLiteral("session/load"),
                sessionParams(true), Request::LoadSession);
    } else {
        if (!_restoreSessionId.isEmpty()) {
            fail(tr("Agent does not support resuming its previous session."));
            return;
        }
        request(QStringLiteral("session/new"),
                sessionParams(false), Request::NewSession);
    }
    _phaseTimeout.start(120000);
}

void AcpClient::sendQueuedPrompt()
{
    if (!_ready || !_busy || _queuedPrompt.isEmpty())
        return;
    const QString promptText = std::exchange(_queuedPrompt, QString());
    request(QStringLiteral("session/prompt"), QJsonObject {
        {QStringLiteral("sessionId"), _sessionId},
        {QStringLiteral("prompt"), QJsonArray{textBlock(promptText)}}
    }, Request::Prompt);
    emit statusChanged(tr("Agent is working…"));
}

void AcpClient::onStdout()
{
    _stdout += _process.readAllStandardOutput();
    if (_stdout.size() > MaxMessageBytes) {
        fail(tr("ACP agent sent an oversized protocol message."));
        return;
    }
    while (!_failed) {
        const int newline = _stdout.indexOf('\n');
        if (newline < 0)
            break;
        QByteArray line = _stdout.left(newline);
        _stdout.remove(0, newline + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (!document.isObject()) {
            fail(tr("ACP stdout contains invalid JSON-RPC (parse error %1).")
                     .arg(int(parseError.error)));
            return;
        }
        handle(document.object());
    }
}

void AcpClient::handle(const QJsonObject& message)
{
    if (message.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0"))
        return;

    const QJsonValue method = message.value(QStringLiteral("method"));
    if (method.isString()) {
        const QString name = method.toString();
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        if (name == QStringLiteral("session/update")) {
            if (!_sessionId.isEmpty() &&
                params.value(QStringLiteral("sessionId")).toString() == _sessionId)
                handleUpdate(params.value(QStringLiteral("update")).toObject());
            return;
        }
        if (!message.contains(QStringLiteral("id")))
            return; // Other notifications require no response.

        const QJsonValue id = message.value(QStringLiteral("id"));
        if (name == QStringLiteral("session/request_permission")) {
            if (_busy && !_cancelled &&
                params.value(QStringLiteral("sessionId")).toString() == _sessionId) {
                _permissions.insert(permissionKey(id), id);
                emit permissionRequested(id, params);
            } else {
                send(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                 {QStringLiteral("id"), id},
                                 {QStringLiteral("result"), cancelledOutcome()}});
            }
            return;
        }
        // We have not advertised fs/terminal capabilities. Never perform
        // arbitrary operations on the client's machine.
        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("error"), QJsonObject{
                {QStringLiteral("code"), -32601},
                {QStringLiteral("message"), QStringLiteral("Method not supported")}
            }}
        });
        return;
    }

    const QJsonValue idValue = message.value(QStringLiteral("id"));
    if (!idValue.isDouble())
        return;
    const qint64 id = static_cast<qint64>(idValue.toDouble());
    auto it = _requests.find(id);
    if (it == _requests.end())
        return;
    const Request kind = it.value();
    _requests.erase(it);

    const QJsonValue error = message.value(QStringLiteral("error"));
    if (error.isObject()) {
        const QString detail = error.toObject()
            .value(QStringLiteral("message")).toString().left(250);
        fail(tr("ACP %1 failed: %2")
             .arg(kind == Request::Initialize ? QStringLiteral("initialize") :
                  kind == Request::NewSession ? QStringLiteral("session/new") :
                  kind == Request::Prompt ? QStringLiteral("session/prompt") :
                  QStringLiteral("session/resume"), detail));
        return;
    }
    if (!message.value(QStringLiteral("result")).isObject()) {
        fail(tr("ACP response has no result object."));
        return;
    }
    handleResult(kind, message.value(QStringLiteral("result")).toObject());
}

void AcpClient::handleResult(Request kind, const QJsonObject& result)
{
    if (kind == Request::Initialize) {
        _phaseTimeout.stop();
        if (result.value(QStringLiteral("protocolVersion")).toInt(-1) != 1) {
            fail(tr("The agent does not support ACP protocol version 1."));
            return;
        }
        const QJsonObject capabilities =
            result.value(QStringLiteral("agentCapabilities")).toObject();
        _canLoad = capabilities.value(QStringLiteral("loadSession")).toBool();
        const QJsonObject sessionCaps =
            capabilities.value(QStringLiteral("sessionCapabilities")).toObject();
        _canResume = sessionCaps.value(QStringLiteral("resume")).isObject();
        _canClose = sessionCaps.value(QStringLiteral("close")).isObject();
        startSession();
    } else if (kind == Request::NewSession ||
               kind == Request::ResumeSession || kind == Request::LoadSession) {
        _phaseTimeout.stop();
        const QString id = kind == Request::NewSession
            ? result.value(QStringLiteral("sessionId")).toString()
            : _restoreSessionId;
        if (id.isEmpty()) {
            fail(tr("ACP agent returned an empty session ID."));
            return;
        }
        _sessionId = id;
        _ready = true;
        emit readyChanged(true);
        emit statusChanged(tr("ACP session ready"));
        sendQueuedPrompt();
    } else if (kind == Request::Prompt) {
        _busy = false;
        _cancelled = false;
        ++_cancelEpoch;
        const QString reason = result.value(QStringLiteral("stopReason")).toString();
        emit turnFinished(reason.isEmpty() ? QStringLiteral("unknown") : reason);
    }
}

void AcpClient::handleUpdate(const QJsonObject& update)
{
    const QString kind = update.value(QStringLiteral("sessionUpdate")).toString();
    if (kind == QStringLiteral("agent_message_chunk")) {
        if (_busy) {
            const QJsonObject content = update.value(QStringLiteral("content")).toObject();
            if (content.value(QStringLiteral("type")).toString() == QStringLiteral("text"))
                emit messageChunk(content.value(QStringLiteral("text")).toString());
        }
    } else if (kind == QStringLiteral("agent_thought_chunk")) {
        const QJsonObject content = update.value(QStringLiteral("content")).toObject();
        if (_busy && content.value(QStringLiteral("type")).toString() == QStringLiteral("text"))
            emit thoughtChunk(content.value(QStringLiteral("text")).toString());
    } else if (kind == QStringLiteral("tool_call") ||
               kind == QStringLiteral("tool_call_update")) {
        const QString toolId = update.value(QStringLiteral("toolCallId")).toString();
        QString title = update.value(QStringLiteral("title")).toString();
        if (!title.isEmpty() && !toolId.isEmpty())
            _toolTitles.insert(toolId, title);
        if (title.isEmpty())
            title = _toolTitles.value(toolId);
        emit toolChanged(title, update.value(QStringLiteral("status")).toString());
    } else if (kind == QStringLiteral("current_mode_update")) {
        emit statusChanged(tr("Agent mode: %1")
                               .arg(update.value(QStringLiteral("currentModeId")).toString()));
    }
}

QString AcpClient::permissionKey(const QJsonValue& id)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact));
}

void AcpClient::answerPermission(const QJsonValue& requestId,
                                 const QString& optionId)
{
    const QString key = permissionKey(requestId);
    if (!_permissions.contains(key))
        return;
    _permissions.remove(key);
    const QJsonObject outcome = optionId.isEmpty()
        ? cancelledOutcome()
        : QJsonObject{{QStringLiteral("outcome"), QJsonObject{
            {QStringLiteral("outcome"), QStringLiteral("selected")},
            {QStringLiteral("optionId"), optionId}
        }}};
    send(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), requestId},
        {QStringLiteral("result"), outcome}
    });
}

void AcpClient::cancelPermissions()
{
    const auto ids = _permissions.values();
    _permissions.clear();
    for (const QJsonValue& id : ids)
        send(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("result"), cancelledOutcome()}
        });
}

void AcpClient::cancel()
{
    if (!_busy || _cancelled)
        return;
    _cancelled = true;
    cancelPermissions();
    if (_sessionId.isEmpty()) {
        fail(tr("Agent initialization cancelled."));
        return;
    }
    notify(QStringLiteral("session/cancel"),
           QJsonObject{{QStringLiteral("sessionId"), _sessionId}});
    emit statusChanged(tr("Cancelling agent turn…"));
    const quint64 cancelEpoch = ++_cancelEpoch;
    QTimer::singleShot(15000, this, [this, cancelEpoch] {
        if (_cancelEpoch == cancelEpoch && _cancelled && _busy && !_failed)
            fail(tr("ACP agent did not acknowledge cancellation."));
    });
}

void AcpClient::stop()
{
    _stopping = true;
    _phaseTimeout.stop();
    cancelPermissions();
    if (_process.state() != QProcess::NotRunning) {
        if (_ready && !_sessionId.isEmpty() && _canClose) {
            request(QStringLiteral("session/close"),
                    QJsonObject{{QStringLiteral("sessionId"), _sessionId}},
                    Request::NewSession);
            _process.waitForBytesWritten(150);
        }
        _process.terminate();
        if (!_process.waitForFinished(350)) {
            _process.kill();
            _process.waitForFinished(350);
        }
    }
    _requests.clear();
    _permissions.clear();
    _toolTitles.clear();
    ++_cancelEpoch;
    _ready = false;
    _busy = false;
    _cancelled = false;
}

void AcpClient::fail(const QString& message)
{
    if (_failed || _stopping)
        return;
    _failed = true;
    _phaseTimeout.stop();
    cancelPermissions();
    _ready = false;
    _busy = false;
    _queuedPrompt.clear();
    if (_process.state() != QProcess::NotRunning)
        _process.kill();
    emit readyChanged(false);
    emit failed(message);
}

} // namespace Mattermost
