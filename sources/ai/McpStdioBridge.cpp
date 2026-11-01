// Lightweight console MCP stdio frontend. Never starts another MatterLeast GUI
// or loads account credentials. Requests are authorized by the running client.
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcessEnvironment>

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString endpoint = env.value(QStringLiteral("MATTERLEAST_MCP_SOCKET"));
    const QString token = env.value(QStringLiteral("MATTERLEAST_MCP_TOKEN"));
    if (endpoint.isEmpty() || token.isEmpty()) {
        std::cerr << "MatterLeast MCP: missing session credentials\n";
        return 1;
    }
    QLocalSocket socket;
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(3000)) {
        std::cerr << "MatterLeast MCP: the desktop client is not available\n";
        return 1;
    }
    QByteArray pending;
    std::string request;
    while (std::getline(std::cin, request)) {
        if (request.size() > 1024 * 1024) {
            std::cerr << "MatterLeast MCP: request is too large\n";
            return 2;
        }
        const QByteArray input(request.data(), int(request.size()));
        const QJsonDocument parsed = QJsonDocument::fromJson(input);
        if (!parsed.isObject())
            continue;
        const QJsonObject rpc = parsed.object();
        const QJsonObject wrapper {
            {QStringLiteral("token"), token},
            {QStringLiteral("mcp"), rpc}
        };
        const QByteArray wire = QJsonDocument(wrapper).toJson(QJsonDocument::Compact) + '\n';
        if (socket.write(wire) < 0 || !socket.waitForBytesWritten(3000)) {
            std::cerr << "MatterLeast MCP: local transport closed\n";
            return 1;
        }
        // MCP notifications have no response. Do not block on initialized.
        if (!rpc.contains(QStringLiteral("id")))
            continue;
        bool received = false;
        while (!received) {
            const int end = pending.indexOf('\n');
            if (end >= 0) {
                const QByteArray response = pending.left(end);
                pending.remove(0, end + 1);
                std::cout.write(response.constData(), response.size());
                std::cout.put('\n');
                std::cout.flush();
                received = true;
                break;
            }
            if (!socket.waitForReadyRead(120000)) {
                std::cerr << "MatterLeast MCP: request timed out\n";
                return 1;
            }
            pending += socket.readAll();
        }
    }
    return 0;
}
