#include <QtTest>

#include <QHostAddress>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/HTTPConnector.h"
#include "backend/NetworkRequest.h"
#include "backend/NetworkSessionContext.h"

using namespace Mattermost;

namespace {

class CookieServer : public QTcpServer
{
public:
    struct Received {
        QByteArray path;
        QByteArray cookie;
    };
    QVector<Received> received;

    CookieServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QTcpSocket::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray data = socket->property("requestData").toByteArray();
                    data += socket->readAll();
                    socket->setProperty("requestData", data);
                    if (!data.contains("\r\n\r\n")
                        || socket->property("replied").toBool()) {
                        return;
                    }
                    socket->setProperty("replied", true);
                    const int pathEnd = data.indexOf(" HTTP/");
                    if (pathEnd < 4) return;
                    Received receivedRequest;
                    receivedRequest.path = data.mid(4, pathEnd - 4);
                    for (const QByteArray& line : data.split('\n')) {
                        if (line.left(7).compare("Cookie:", Qt::CaseInsensitive) == 0)
                            receivedRequest.cookie = line.mid(7).trimmed();
                    }
                    received.append(receivedRequest);
                    const QByteArray label = receivedRequest.path.contains("one")
                        ? QByteArrayLiteral("one")
                        : receivedRequest.path.contains("two")
                            ? QByteArrayLiteral("two")
                            : QByteArrayLiteral("legacy");
                    const QByteArray body = QByteArrayLiteral("{}");
                    socket->write(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Set-Cookie: MMUSERID=" + label +
                        "\r\nConnection: close\r\nContent-Length: 2\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QByteArray cookieFor(const QByteArray& path) const
    {
        for (const Received& request : received) {
            if (request.path == path)
                return request.cookie;
        }
        return {};
    }
};

} // namespace

class HttpConnectorSessionTest : public QObject
{
    Q_OBJECT

private slots:
    void simultaneousContextsDoNotShareAuthentication()
    {
        CookieServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString host = QStringLiteral("http://127.0.0.1:%1/")
            .arg(server.serverPort());

        NetworkSessionContext firstContext;
        NetworkSessionContext secondContext;
        firstContext.setHost(host);
        secondContext.setHost(host);
        firstContext.setToken(QStringLiteral("first-token"));
        secondContext.setToken(QStringLiteral("second-token"));
        HTTPConnector first(&firstContext);
        HTTPConnector second(&secondContext);
        first.configureAccountCache(QStringLiteral("first-account"));
        second.configureAccountCache(QStringLiteral("second-account"));

        int completed = 0;
        NetworkRequest firstRequest(firstContext, QStringLiteral("probe/one"));
        NetworkRequest secondRequest(secondContext, QStringLiteral("probe/two"));
        first.get(firstRequest, HttpResponseCallback(
            [&completed](QByteArray) { ++completed; }));
        second.get(secondRequest, HttpResponseCallback(
            [&completed](QByteArray) { ++completed; }));
        QTRY_COMPARE(completed, 2);
        QCOMPARE(server.received.size(), 2);

        const QByteArray one = server.cookieFor("/api/v4/probe/one");
        const QByteArray two = server.cookieFor("/api/v4/probe/two");
        QVERIFY(one.contains("MMAUTHTOKEN=first-token"));
        QVERIFY(!one.contains("second-token"));
        QVERIFY(two.contains("MMAUTHTOKEN=second-token"));
        QVERIFY(!two.contains("first-token"));

        // Set-Cookie on one response must never mutate the other context.
        const QByteArray nextOne =
            NetworkRequest(firstContext, QStringLiteral("users/me"))
                .rawHeader("Cookie");
        const QByteArray nextTwo =
            NetworkRequest(secondContext, QStringLiteral("users/me"))
                .rawHeader("Cookie");
        QVERIFY(nextOne.contains("MMUSERID=one"));
        QVERIFY(!nextOne.contains("MMUSERID=two"));
        QVERIFY(nextTwo.contains("MMUSERID=two"));
        QVERIFY(!nextTwo.contains("MMUSERID=one"));

        // A caller that accidentally constructed a legacy-global request
        // cannot inject the global token into a bound connector.
        NetworkRequest::setHost(host);
        NetworkRequest::setToken(QStringLiteral("legacy-token"));
        NetworkRequest legacyRequest(QStringLiteral("probe/legacy"));
        first.get(legacyRequest, HttpResponseCallback(
            [&completed](QByteArray) { ++completed; }));
        QTRY_COMPARE(completed, 3);
        const QByteArray third = server.cookieFor("/api/v4/probe/legacy");
        QVERIFY2(third.contains("MMAUTHTOKEN=first-token"),
                 qPrintable(QStringLiteral("Unexpected outgoing cookie: %1")
                     .arg(QString::fromLatin1(third))));
        QVERIFY2(!third.contains("legacy-token"),
                 "A legacy-global token reached a bound session connector");
        NetworkRequest::clearToken();
    }

    void redirectedOriginCannotModifyContextCookies()
    {
        NetworkSessionContext context;
        context.setHost(QStringLiteral("https://first.example.test/"));
        context.setToken(QStringLiteral("secret"));
        QVERIFY(!context.updateSessionCookiesFrom(
            QUrl(QStringLiteral("https://second.example.test/api/v4/users/me")),
            {QNetworkCookie(QByteArrayLiteral("MMUSERID"),
                            QByteArrayLiteral("wrong-user"))}));
        QVERIFY(context.updateSessionCookiesFrom(
            QUrl(QStringLiteral("https://first.example.test/api/v4/users/me")),
            {QNetworkCookie(QByteArrayLiteral("MMUSERID"),
                            QByteArrayLiteral("right-user"))}));
        const QByteArray cookies = NetworkRequest(
            context, QStringLiteral("users/me")).rawHeader("Cookie");
        QVERIFY(cookies.contains("MMUSERID=right-user"));
        QVERIFY(!cookies.contains("wrong-user"));
    }
};

QTEST_GUILESS_MAIN(HttpConnectorSessionTest)
#include "HttpConnectorSessionTest.moc"
