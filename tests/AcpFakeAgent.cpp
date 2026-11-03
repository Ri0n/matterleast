// Deterministic ACP v1 stdio peer for integration tests. No model/network needed.
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <string>

namespace {
void send(const QJsonObject& value)
{
    const QByteArray wire = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::cout.write(wire.constData(), wire.size());
    std::cout.put('\n');
    std::cout.flush();
}

void result(const QJsonValue& id, const QJsonObject& value)
{
    send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}

void error(const QJsonValue& id, const QString& message)
{
    send({{"jsonrpc", "2.0"}, {"id", id},
          {"error", QJsonObject{{"code", -32001}, {"message", message}}}});
}

void update(const QString& kind, const QJsonObject& fields)
{
    QJsonObject data = fields;
    data.insert("sessionUpdate", kind);
    send({{"jsonrpc", "2.0"}, {"method", "session/update"},
          {"params", QJsonObject{{"sessionId", "test-session"}, {"update", data}}}});
}

void chunk(const QString& text)
{
    update("agent_message_chunk",
           {{"content", QJsonObject{{"type", "text"}, {"text", text}}}});
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QJsonValue pendingPrompt;
    QString pendingScenario;
    std::string raw;
    while (std::getline(std::cin, raw)) {
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray(raw.data(), int(raw.size())));
        if (!doc.isObject())
            continue;
        const QJsonObject message = doc.object();
        const QString method = message.value("method").toString();
        const QJsonValue id = message.value("id");
        const QJsonObject params = message.value("params").toObject();

        if (method == "initialize") {
            if (params.value("protocolVersion").toInt() != 1 ||
                params.value("clientCapabilities").toObject()
                      .contains("terminal")) {
                error(id, "unsupported client protocol or capability");
                continue;
            }
            result(id, QJsonObject{
                {"protocolVersion", 1},
                {"agentCapabilities", QJsonObject{
                    {"loadSession", true},
                    {"sessionCapabilities", QJsonObject{
                        {"resume", QJsonObject{}}, {"close", QJsonObject{}}
                    }}
                }},
                {"authMethods", QJsonArray{}}
            });
        } else if (method == "session/new" ||
                   method == "session/resume" ||
                   method == "session/load") {
            const QJsonArray servers = params.value("mcpServers").toArray();
            if (servers.size() != 1 ||
                servers.at(0).toObject().value("name").toString() != "matterleast" ||
                servers.at(0).toObject().value("command").toString().isEmpty() ||
                !servers.at(0).toObject().value("args").isArray() ||
                params.value("cwd").toString().isEmpty()) {
                error(id, "invalid session MCP configuration");
                continue;
            }
            const QJsonArray env = servers.at(0).toObject().value("env").toArray();
            bool hasEndpoint = false, hasToken = false;
            for (const QJsonValue& item : env) {
                const QJsonObject value = item.toObject();
                hasEndpoint |= value.value("name").toString() == "MATTERLEAST_MCP_SOCKET"
                    && value.value("value").toString() == "local-endpoint";
                hasToken |= value.value("name").toString() == "MATTERLEAST_MCP_TOKEN"
                    && value.value("value").toString() == "secret";
            }
            if (!hasEndpoint || !hasToken) {
                error(id, "missing MCP environment variables");
                continue;
            }
            if (method != "session/new" &&
                params.value("sessionId").toString() != "test-session") {
                error(id, "wrong restore session");
                continue;
            }
            result(id, method == "session/new"
                ? QJsonObject{{"sessionId", "test-session"}}
                : QJsonObject{});
        } else if (method == "session/prompt") {
            const QJsonArray blocks = params.value("prompt").toArray();
            if (params.value("sessionId").toString() != "test-session" ||
                blocks.isEmpty() ||
                blocks.at(0).toObject().value("type").toString() != "text") {
                error(id, "invalid prompt");
                continue;
            }
            const QString text = blocks.at(0).toObject().value("text").toString();
            if (text.contains("permission")) {
                pendingPrompt = id;
                pendingScenario = "permission";
                update("tool_call", {{"toolCallId", "c1"},
                                     {"title", "Post a reply"},
                                     {"status", "pending"}});
                send({{"jsonrpc", "2.0"}, {"id", 77},
                      {"method", "session/request_permission"},
                      {"params", QJsonObject{
                          {"sessionId", "test-session"},
                          {"toolCall", QJsonObject{
                              {"toolCallId", "c1"}, {"title", "Post a reply"}
                          }},
                          {"options", QJsonArray{
                              QJsonObject{{"optionId", "allow"}, {"name", "Allow once"},
                                          {"kind", "allow_once"}},
                              QJsonObject{{"optionId", "deny"}, {"name", "Deny"},
                                          {"kind", "reject_once"}}
                          }}
                      }}});
            } else if (text.contains("cancel")) {
                pendingPrompt = id;
                pendingScenario = "cancel";
            } else if (text.contains("unsupported")) {
                pendingPrompt = id;
                pendingScenario = "unsupported";
                send({{"jsonrpc", "2.0"}, {"id", 99},
                      {"method", "fs/read_text_file"},
                      {"params", QJsonObject{{"path", "/tmp/test"}}}});
            } else {
                update("agent_thought_chunk",
                       {{"content", QJsonObject{{"type", "text"}, {"text", "thinking"}}}});
                update("tool_call", {{"toolCallId", "c1"},
                                     {"title", "Read MatterLeast thread"},
                                     {"status", "completed"}});
                chunk("hello ");
                chunk("there");
                result(id, {{"stopReason", "end_turn"}});
            }
        } else if (method == "session/cancel") {
            if (!pendingPrompt.isUndefined() && !pendingPrompt.isNull()) {
                result(pendingPrompt, {{"stopReason", "cancelled"}});
                pendingPrompt = QJsonValue();
                pendingScenario.clear();
            }
        } else if (method == "session/close") {
            result(id, {});
        } else if (method.isEmpty()) {
            if (pendingScenario == "permission" && id.toInt() == 77) {
                const QJsonObject outcome = message.value("result").toObject()
                                                .value("outcome").toObject();
                chunk(outcome.value("outcome").toString() == "selected"
                          && outcome.value("optionId").toString() == "allow"
                      ? QStringLiteral("approved") : QStringLiteral("rejected"));
                result(pendingPrompt, {{"stopReason", "end_turn"}});
                pendingPrompt = QJsonValue();
                pendingScenario.clear();
            } else if (pendingScenario == "unsupported" && id.toInt() == 99) {
                if (message.value("error").toObject().value("code").toInt() == -32601)
                    chunk("unsupported-method-denied");
                result(pendingPrompt, {{"stopReason", "end_turn"}});
                pendingPrompt = QJsonValue();
                pendingScenario.clear();
            }
        }
    }
    return 0;
}
