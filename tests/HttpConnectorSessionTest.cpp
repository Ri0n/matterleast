#include <QtTest>

#include <QHostAddress>
#include <QNetworkReply>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/HTTPConnector.h"
#include "backend/LocalPostDeleteTracker.h"
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
    bool holdResponses = false;
    QVector<QPair<QPointer<QTcpSocket>, QByteArray>> pendingResponses;

    void releaseHeldResponses()
    {
        auto held = std::move(pendingResponses);
        pendingResponses.clear();
        for (const auto& entry : held) {
            if (entry.first) {
                entry.first->write(entry.second);
                entry.first->disconnectFromHost();
            }
        }
    }

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
                    const QByteArray response =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Set-Cookie: MMUSERID=" + label +
                        "\r\nConnection: close\r\nContent-Length: 2\r\n\r\n" + body;
                    if (holdResponses) {
                        pendingResponses.append(qMakePair(QPointer<QTcpSocket>(socket), response));
                    } else {
                        socket->write(response);
                        socket->disconnectFromHost();
                    }
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

        // Even incorrect, caller-supplied credentials on a raw request
        // must be discarded by the owning account's connector.
        QNetworkRequest wrongHeaders(QUrl(host + QStringLiteral("api/v4/probe/legacy")));
        wrongHeaders.setRawHeader("Cookie", "MMAUTHTOKEN=legacy-token");
        wrongHeaders.setRawHeader("Authorization", "Bearer legacy-token");
        wrongHeaders.setRawHeader("X-CSRF-Token", "legacy-csrf");
        first.get(wrongHeaders, HttpResponseCallback(
            [&completed](QByteArray) { ++completed; }));
        QTRY_COMPARE(completed, 3);
        const QByteArray third = server.cookieFor("/api/v4/probe/legacy");
        QVERIFY2(third.contains("MMAUTHTOKEN=first-token"),
                 qPrintable(QStringLiteral("Unexpected outgoing cookie: %1")
                     .arg(QString::fromLatin1(third))));
        QVERIFY2(!third.contains("legacy-token"),
                 "Wrong caller credentials reached the bound session connector");
    }

    void anonymousConnectorDoesNotReplayResponseCookies()
    {
        CookieServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString host = QStringLiteral("http://127.0.0.1:%1/")
            .arg(server.serverPort());
        HTTPConnector anonymous;
        int completed = 0;
        QNetworkRequest first(QUrl(host + QStringLiteral("api/v4/probe/anonymous-first")));
        anonymous.get(first, HttpResponseCallback(
            [&](QByteArray) { ++completed; }));
        QTRY_COMPARE(completed, 1);
        // The server sets MMUSERID on every response; it must not be
        // implicitly replayed by an unbound connector.
        QNetworkRequest second(QUrl(host + QStringLiteral("api/v4/probe/anonymous-second")));
        anonymous.get(second, HttpResponseCallback(
            [&](QByteArray) { ++completed; }));
        QTRY_COMPARE(completed, 2);
        QVERIFY(server.cookieFor("/api/v4/probe/anonymous-first").isEmpty());
        QVERIFY(server.cookieFor("/api/v4/probe/anonymous-second").isEmpty());
    }

    void postDeletionTrackingDoesNotCrossHttpConnectors()
    {
        CookieServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString host = QStringLiteral("http://127.0.0.1:%1/")
            .arg(server.serverPort());
        NetworkSessionContext firstContext;
        NetworkSessionContext secondContext;
        firstContext.setHost(host);
        secondContext.setHost(host);
        firstContext.setToken(QStringLiteral("account-one-token"));
        secondContext.setToken(QStringLiteral("account-two-token"));

        LocalPostDeleteTracker firstTracker;
        LocalPostDeleteTracker secondTracker;
        HTTPConnector first(&firstContext, &firstTracker);
        HTTPConnector second(&secondContext, &secondTracker);

        const QString id = QStringLiteral("shared-post-id");
        NetworkRequest firstDelete(firstContext, QStringLiteral("posts/") + id);
        NetworkRequest secondDelete(secondContext, QStringLiteral("posts/") + id);
        first.del(firstDelete);
        second.del(secondDelete);
        QTRY_COMPARE(server.received.size(), 2);

        // Reset one transport before consuming its websocket delete event.
        first.reset();
        QVERIFY(!firstTracker.take(id));
        QVERIFY(secondTracker.take(id));

        // The same ID remains one-shot inside each separate tracker.
        firstTracker.noteRequest(firstDelete.url());
        secondTracker.noteRequest(secondDelete.url());
        QVERIFY(firstTracker.take(id));
        QVERIFY(!firstTracker.take(id));
        QVERIFY(secondTracker.take(id));
    }

    void accountResetCancelsOnlyItsBoundConnectors()
    {
        CookieServer server;
        server.holdResponses = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString host = QStringLiteral("http://127.0.0.1:%1/")
            .arg(server.serverPort());

        NetworkSessionContext firstAccount;
        NetworkSessionContext secondAccount;
        firstAccount.setHost(host);
        secondAccount.setHost(host);
        firstAccount.setToken(QStringLiteral("first-token"));
        secondAccount.setToken(QStringLiteral("second-token"));

        // Two independent service connectors share the first account, while
        // the third belongs to an unrelated Backend on the same test server.
        HTTPConnector firstService(&firstAccount);
        HTTPConnector secondService(&firstAccount);
        HTTPConnector otherAccount(&secondAccount);
        int firstCompletions = 0;
        int secondCompletions = 0;
        int otherCompletions = 0;
        NetworkRequest firstRequest(firstAccount, QStringLiteral("probe/first"));
        NetworkRequest secondRequest(firstAccount, QStringLiteral("probe/second"));
        NetworkRequest otherRequest(secondAccount, QStringLiteral("probe/other"));
        firstService.get(firstRequest, HttpResponseCallback(
            [&](QByteArray) { ++firstCompletions; }));
        secondService.get(secondRequest, HttpResponseCallback(
            [&](QByteArray) { ++secondCompletions; }));
        otherAccount.get(otherRequest, HttpResponseCallback(
            [&](QByteArray) { ++otherCompletions; }));
        QTRY_COMPARE(server.received.size(), 3);

        HTTPConnector::resetSessionTransports(&firstAccount);
        server.releaseHeldResponses();
        QTRY_COMPARE(otherCompletions, 1);
        QCOMPARE(firstCompletions, 0);
        QCOMPARE(secondCompletions, 0);
        QVERIFY(server.cookieFor("/api/v4/probe/other").contains(
            "MMAUTHTOKEN=second-token"));

        // Reset leaves both the first account's connectors reusable.
        server.holdResponses = false;
        NetworkRequest retry(firstAccount, QStringLiteral("probe/reused"));
        firstService.get(retry, HttpResponseCallback(
            [&](QByteArray) { ++firstCompletions; }));
        QTRY_COMPARE(firstCompletions, 1);
        QCOMPARE(secondCompletions, 0);
    }

    void reconnectOfOneAccountDoesNotRestartOtherAccount()
    {
        CookieServer server;
        server.holdResponses = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString host = QStringLiteral("http://127.0.0.1:%1/")
            .arg(server.serverPort());

        NetworkSessionContext reconnecting;
        NetworkSessionContext unaffected;
        reconnecting.setHost(host);
        unaffected.setHost(host);
        reconnecting.setToken(QStringLiteral("first-token"));
        unaffected.setToken(QStringLiteral("second-token"));
        HTTPConnector first(&reconnecting);
        HTTPConnector second(&unaffected);

        int firstCompleted = 0;
        int secondCompleted = 0;
        NetworkRequest firstRequest(reconnecting, QStringLiteral("probe/restart-one"));
        NetworkRequest secondRequest(unaffected, QStringLiteral("probe/keep-two"));
        first.get(firstRequest, HttpResponseCallback(
            [&](QByteArray) { ++firstCompleted; }));
        second.get(secondRequest, HttpResponseCallback(
            [&](QByteArray) { ++secondCompleted; }));
        QTRY_COMPARE(server.received.size(), 2);

        HTTPConnector::restartSessionTransports(&reconnecting);
        server.holdResponses = false;
        server.releaseHeldResponses();
        QTRY_COMPARE(firstCompleted, 1);
        QTRY_COMPARE(secondCompleted, 1);

        int peerRequests = 0;
        for (const CookieServer::Received& request : server.received) {
            if (request.path == QByteArrayLiteral("/api/v4/probe/keep-two")) {
                ++peerRequests;
                QVERIFY(request.cookie.contains("MMAUTHTOKEN=second-token"));
                QVERIFY(!request.cookie.contains("first-token"));
            }
        }
        // A global restart would replay the unrelated account's GET and
        // produce a second request to /probe/keep-two.
        QCOMPARE(peerRequests, 1);
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
