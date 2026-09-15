#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLoggingCategory>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ThreadPostSource.h"
#include "widgets/LongListWidget.h"

using namespace Mattermost;

namespace {
QString id(int index) { return QStringLiteral("post%1").arg(index, 22, 10, QLatin1Char('0')); }
qint64 timestamp(int index)
{
    // Deliberately non-uniform posting density: timestamp interpolation is not rank.
    return 1000000000000LL + qint64(index) * index * index * index;
}
QJsonObject post(int index)
{
    QJsonObject value {{"id", id(index)}, {"channel_id", "channel"},
                       {"root_id", index ? id(0) : QString()},
                       {"user_id", "user"}, {"message", QString::number(index)},
                       {"create_at", timestamp(index)}, {"update_at", timestamp(index)}};
    if (!index) {
        value.insert("reply_count", 731);
        value.insert("last_reply_at", timestamp(731));
    }
    return value;
}

// Real HTTP endpoint: applies Mattermost's exclusive (create_at, id) cursor,
// includes the root in every response, and reports has_next in fetch direction.
class ThreadServer : public QTcpServer
{
public:
    QList<QUrl> requests;
    int pageLimit = 1000;
    int responseDelayMs = 0;
    int returnedReplies = 0;
    qint64 responseBytes = 0;
    int largestRequestedPage = 0;
    bool failNext = false;
    bool emptyNext = false;
    int replyCount = 731;
    bool omitHasNext = false;
    bool omitLastReplyAt = false;
    bool emptyAtBoundary = false;
    explicit ThreadServer(QObject* parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray input = socket->property("input").toByteArray() + socket->readAll();
                    socket->setProperty("input", input);
                    if (!input.contains("\r\n\r\n") || socket->property("replied").toBool()) return;
                    socket->setProperty("replied", true);
                    QUrl url(QString::fromUtf8(input.split(' ').value(1)));
                    requests.push_back(url);
                    QUrlQuery query(url);
                    const bool backward = query.queryItemValue("direction") == "up";
                    const QString anchor = query.queryItemValue("fromPost");
                    const qint64 time = query.queryItemValue("fromCreateAt").toLongLong();
                    largestRequestedPage = std::max(largestRequestedPage, query.queryItemValue("perPage").toInt());
                    const int limit = std::min(pageLimit, query.queryItemValue("perPage").toInt());
                    QList<int> selected;
                    for (int i = 1; i <= replyCount; ++i) {
                        const bool after = timestamp(i) > time || (timestamp(i) == time && id(i) > anchor);
                        const bool before = timestamp(i) < time || (timestamp(i) == time && id(i) < anchor);
                        if (!time || (backward ? before : after)) selected.push_back(i);
                    }
                    if (backward) std::reverse(selected.begin(), selected.end());
                    const bool hasNext = !emptyAtBoundary && selected.size() > limit;
                    selected = selected.mid(0, limit);
                    if (emptyNext || emptyAtBoundary) { selected.clear(); emptyNext = false; }
                    QJsonObject root = post(0);
                    root.insert("reply_count", replyCount);
                    root.insert("last_reply_at", timestamp(replyCount));
                    if (omitLastReplyAt) root.remove("last_reply_at");
                    QJsonObject posts {{id(0), root}};
                    QJsonArray order {id(0)};
                    for (int i : selected) { posts.insert(id(i), post(i)); order.append(id(i)); }
                    QJsonObject response {{"posts", posts}, {"order", order}};
                    if (!omitHasNext) response.insert("has_next", hasNext);
                    const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    const QByteArray status = failNext ? "500 Internal Server Error" : "200 OK";
                    failNext = false;
                    returnedReplies += selected.size();
                    responseBytes += body.size();
                    const QByteArray responseData = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\n\r\n" + body;
                    QTimer::singleShot(responseDelayMs, socket, [socket, responseData] {
                        socket->write(responseData);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
};

class SourceList : public LongListWidget
{
public:
    explicit SourceList(ThreadPostSource& source, bool fixedRows = false) : source(source), fixedRows(fixedRows)
    {
        setDefaultItemHeight(40);
        setPrefetchScreens(0);
        setSeekDebounceMs(0);
        setItemCount(source.itemCount());
        connect(this, &LongListWidget::rangeRequested, &source,
                [&source](int first, int last, RequestReason reason, quint64 generation) {
            source.requestRange(first, last, static_cast<AbstractPostSource::RequestReason>(reason), generation);
        });
        connect(&source, &AbstractPostSource::rangeAvailable, this, [this](int first, int last) {
            for (int i = first; i <= last; ++i) setRangeAvailable(i, i, this->source.isAvailable(i));
        });
        connect(&source, &AbstractPostSource::bodyAvailabilityChanged, this,
                [this](int first, int last, bool isAvailable) { setRangeAvailable(first, last, isAvailable); });
        connect(&source, &AbstractPostSource::seekTargetResolved, this, &LongListWidget::resolveSeekTarget);
        connect(&source, &AbstractPostSource::layoutChanged, this, [this](int first, int last) {
            reconcileItemLayout(first, last);
            restoreTarget();
        });
        connect(&source, &AbstractPostSource::rangeAvailable, this, [this] { restoreTarget(); });
        connect(&source, &AbstractPostSource::rangeRequestFinished, this, [this](int first, int last) {
            // Match ChatLogWidget: an already resident range can complete
            // without fetching a page or emitting another rangeAvailable.
            for (int i = std::max(0, first); i <= std::min(last, this->source.itemCount() - 1); ++i) {
                if (this->source.isAvailable(i) && !isItemAvailable(i)) setRangeAvailable(i, i);
            }
            finishRangeRequest(first, last);
        });
        connect(&source, &AbstractPostSource::itemCountChanged, this, &LongListWidget::setItemCount);
    }
    void followPost(const QString& id) { followedId = id; restoreTarget(); }
protected:
    QString itemIdentity(const QWidget* widget) const override
    {
        const auto* label = qobject_cast<const QLabel*>(widget);
        return label ? label->text() : QString();
    }
    int indexOfItemIdentity(const QString& id) const override { return source.indexOfPost(id); }
    bool isModelItemAvailable(int index) const override { return source.isAvailable(index); }
    QWidget* createItemWidget(int index) override
    {
        auto* value = source.postAt(index);
        if (!value) return nullptr;
        auto* label = new QLabel(value->id);
        if (fixedRows) label->setFixedHeight(40);
        return label;
    }
private:
    void restoreTarget()
    {
        const int index = source.indexOfPost(followedId);
        if (index >= 0) scrollToIndex(index, Alignment::Center);
    }
    ThreadPostSource& source;
    bool fixedRows;
    QString followedId;
};
}

class ThreadPostSourceIntegrationTest : public QObject
{
    Q_OBJECT
private slots:
    void cursorDemandUsesExactRanks_data()
    {
        QTest::addColumn<int>("target");
        for (int target : {382, 158, 251, 362}) QTest::newRow(qPrintable(QString::number(target))) << target;
    }
    void cursorDemandUsesExactRanks()
    {
        QFETCH(int, target);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        source.requestRange(436, 731, AbstractPostSource::RequestReason::Scroll, 1);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(source.itemCount(), 732);
        QCOMPARE(source.indexOfPost(id(436)), 436);
        server.pageLimit = 13;
        QVERIFY(channel.evictPostBody(id(436)));
        int evicted = 0;
        connect(&source, &AbstractPostSource::rangeAvailable, &source, [&](int first, int last) {
            for (int i = first; i <= last; ++i) {
                if ((i < target || i > target + 9) && source.postAt(i)) {
                    evicted += channel.evictPostBody(source.postAt(i)->id);
                }
            }
        });
        finished.clear();
        bool prematureFinish = false;
        connect(&source, &AbstractPostSource::rangeRequestFinished, &source, [&](int first, int last) {
            for (int i = first; i <= last; ++i) prematureFinish |= !source.isAvailable(i);
        });
        source.requestRange(target, target + 9, AbstractPostSource::RequestReason::Scroll, 2);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!prematureFinish);
        for (int i = target; i <= target + 9; ++i) {
            QVERIFY(source.postAt(i));
            QCOMPARE(source.postAt(i)->id, id(i));
            QVERIFY(source.isPostPositionAuthoritative(id(i)));
        }
        QVERIFY(evicted > 0);
        QVERIFY(server.requests.size() < 60);
        QSet<QString> cursors;
        for (const auto& request : server.requests) {
            const QUrlQuery query(request);
            const QString anchor = query.queryItemValue("fromPost");
            if (anchor.isEmpty()) {
                QCOMPARE(query.queryItemValue("direction"), QStringLiteral("up"));
            } else {
                const QString key = query.queryItemValue("direction") + anchor;
                QVERIFY(!cursors.contains(key));
                cursors.insert(key);
            }
        }
    }
    void failureIsExplicitAndUserCanRetry_data()
    {
        QTest::addColumn<bool>("transportError");
        QTest::newRow("http-error") << true;
        QTest::newRow("empty-cursor-page") << false;
    }
    void failureIsExplicitAndUserCanRetry()
    {
        QFETCH(bool, transportError);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        server.failNext = transportError;
        server.emptyNext = !transportError;
        source.requestRange(382, 391, AbstractPostSource::RequestReason::Seek, 1);
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(finished.size(), 1);
        QTest::qWait(100);
        QCOMPARE(server.requests.size(), 1);
        source.requestRange(382, 391, AbstractPostSource::RequestReason::Seek, 2);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(failures.size(), 1);
        for (int i = 382; i <= 391; ++i) {
            QVERIFY(source.postAt(i));
            QVERIFY(!source.isPostPositionAuthoritative(source.postAt(i)->id));
        }
    }
    void newestSeekSupersedesInFlightWork()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        source.requestRange(382, 391, AbstractPostSource::RequestReason::Seek, 1);
        source.requestRange(158, 167, AbstractPostSource::RequestReason::Seek, 2);
        source.requestRange(251, 260, AbstractPostSource::RequestReason::Seek, 3);
        QTRY_COMPARE(finished.size(), 3);
        for (int i = 251; i <= 260; ++i) {
            QVERIFY(source.postAt(i));
            QVERIFY(!source.isPostPositionAuthoritative(source.postAt(i)->id));
        }
        QCOMPARE(failures.size(), 0);
        QCOMPARE(server.requests.size(), 3);
        // Ordinary scrolling uses generation zero after a seek.
        source.requestRange(261, 270, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 4);
        QVERIFY(source.isAvailable(270));
    }
    void permalinkCreatesBoundedIsland_data()
    {
        QTest::addColumn<int>("replies");
        QTest::addColumn<int>("target");
        QTest::newRow("732-items") << 731 << 500;
        QTest::newRow("10001-items") << 10000 << 5000;
    }
    void permalinkCreatesBoundedIsland()
    {
        QFETCH(int, replies);
        QFETCH(int, target);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.replyCount = replies;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        QJsonObject root = post(0);
        root.insert("reply_count", replies);
        root.insert("last_reply_at", timestamp(replies));
        channel.addPost(root);
        channel.mergePostContext(QJsonArray {id(target)}, QJsonObject {{id(target), post(target)}});
        ThreadPostSource source(backend, channel, id(0));
        const int estimate = source.ensurePostIndex(id(target));
        QVERIFY(estimate != target);
        QVERIFY(!source.isAvailable(estimate));
        QVERIFY(!source.isPostReadyForNavigation(id(target)));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        source.requestRange(estimate - 4, estimate + 5, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(failures.size(), 0);
        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.returnedReplies, 30);
        QVERIFY(server.responseBytes < 15000);
        QCOMPARE(source.itemCount(), replies + 1);
        const int placed = source.indexOfPost(id(target));
        QVERIFY(source.isAvailable(placed));
        QVERIFY(source.isPostReadyForNavigation(id(target)));
        QVERIFY(!source.isPostPositionAuthoritative(id(target)));
        for (int offset = -15; offset <= 15; ++offset) {
            QVERIFY(source.postAt(placed + offset));
            QCOMPARE(source.postAt(placed + offset)->id, id(target + offset));
        }
        QVERIFY(!source.isAvailable(placed - 16));
        QVERIFY(!source.isAvailable(placed + 16));

        // Scroll extends the semantic edge even after its body is evicted.
        QVERIFY(channel.evictPostBody(id(target - 15)));
        source.requestRange(placed - 25, placed - 16, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(server.requests.size(), 3);
        QCOMPARE(server.returnedReplies, 40);
        QCOMPARE(source.indexOfPost(id(target)), placed);
        QCOMPARE(source.postAt(placed - 25)->id, id(target - 25));
        QVERIFY(!source.isPostPositionAuthoritative(id(target)));
        const QUrlQuery expansion(server.requests.last());
        QCOMPARE(expansion.queryItemValue("fromPost"), id(target - 15));
        QCOMPARE(expansion.queryItemValue("direction"), QStringLiteral("up"));
        QVERIFY(channel.evictPostBody(id(target)));
        QVERIFY(!source.isPostReadyForNavigation(id(target)));
        source.requestRange(placed, placed, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(server.requests.size(), 4);
        QVERIFY(source.isPostReadyForNavigation(id(target)));
        QVERIFY(!source.isPostPositionAuthoritative(id(target)));
    }
    void exactSeekReconcilesIslandByIdentity()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        channel.mergePostContext(QJsonArray {id(500)}, QJsonObject {{id(500), post(500)}});
        ThreadPostSource source(backend, channel, id(0));
        const int estimate = source.ensurePostIndex(id(500));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        source.requestRange(estimate - 4, estimate + 5, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(finished.size(), 1);
        // The exact prefix touches the provisional island numerically, with no
        // identity overlap. It must move the whole island, not promote its rows.
        source.requestRange(1, estimate + 9, AbstractPostSource::RequestReason::Scroll, 1);
        QTRY_COMPARE(finished.size(), 2);
        for (int i = estimate; i <= estimate + 9; ++i) {
            QVERIFY(source.postAt(i));
            QCOMPARE(source.postAt(i)->id, id(i));
        }
        QVERIFY(!source.isPostPositionAuthoritative(id(500)));
        QVERIFY(source.indexOfPost(id(500)) != estimate);
        const int islandFirst = source.indexOfPost(id(485));
        QVERIFY(islandFirst > estimate + 10);
        QCOMPARE(source.indexOfPost(id(515)), islandFirst + 30);
        QVERIFY(!source.isAvailable(islandFirst - 1));
        QVERIFY(!source.isAvailable(islandFirst + 31));
        // Actual identity overlap with an exact cursor page proves the ranks
        // of the entire island, including rows outside that particular page.
        source.requestRange(1, 504, AbstractPostSource::RequestReason::Scroll, 2);
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(failures.size(), 0);
        for (int i = 485; i <= 515; ++i) {
            QCOMPARE(source.indexOfPost(id(i)), i);
            QVERIFY(source.isPostPositionAuthoritative(id(i)));
        }
    }
    void permalinkBoundaryProvesRank_data()
    {
        QTest::addColumn<int>("target");
        QTest::newRow("oldest-reply") << 1;
        QTest::newRow("newest-reply") << 731;
    }
    void permalinkBoundaryProvesRank()
    {
        QFETCH(int, target);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        channel.mergePostContext(QJsonArray {id(target)}, QJsonObject {{id(target), post(target)}});
        ThreadPostSource source(backend, channel, id(0));
        const int estimate = source.ensurePostIndex(id(target));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        source.requestRange(estimate, estimate, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.returnedReplies, 15);
        QCOMPARE(source.itemCount(), 732);
        QCOMPARE(source.indexOfPost(id(target)), target);
        QVERIFY(source.isPostPositionAuthoritative(id(target)));
        QVERIFY(source.isAvailable(target));
    }
    void serverSummaryCorrectsObsoleteCount()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.replyCount = 500;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        source.requestRange(700, 709, AbstractPostSource::RequestReason::Seek, 1);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(source.itemCount(), 501);
        QCOMPARE(source.indexOfPost(id(500)), 500);
        QCOMPARE(failures.size(), 0);
    }
    void tailBootstrapWithoutReliableSummary_data()
    {
        QTest::addColumn<bool>("missingTimestamp");
        QTest::addColumn<bool>("missingHasNext");
        QTest::newRow("missing-last-reply-at") << true << false;
        QTest::newRow("stale-last-reply-at") << false << false;
        QTest::newRow("missing-has-next") << true << true;
    }
    void tailBootstrapWithoutReliableSummary()
    {
        QFETCH(bool, missingTimestamp);
        QFETCH(bool, missingHasNext);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.omitLastReplyAt = true;
        server.omitHasNext = missingHasNext;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        QJsonObject root = post(0);
        if (missingTimestamp) root.remove("last_reply_at");
        else root.insert("last_reply_at", timestamp(400));
        channel.addPost(root);
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        source.requestRange(715, 731, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(source.itemCount(), 732);
        QCOMPARE(failures.size(), 0);
        for (int i = 715; i <= 731; ++i) {
            QVERIFY(source.postAt(i));
            QCOMPARE(source.postAt(i)->id, id(i));
        }
        const QUrlQuery query(server.requests.first());
        QVERIFY(!query.hasQueryItem("fromCreateAt"));
        QVERIFY(!query.hasQueryItem("fromPost"));
        QCOMPARE(query.queryItemValue("direction"), QStringLiteral("up"));
    }
    void emptyPageCannotEraseLogicalThread_data()
    {
        QTest::addColumn<int>("first");
        QTest::newRow("tail") << 715;
        QTest::newRow("forward-from-root") << 1;
    }
    void emptyPageCannotEraseLogicalThread()
    {
        QFETCH(int, first);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.emptyAtBoundary = true;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        source.requestRange(first, first + 9, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(source.itemCount(), 732);
        QCOMPARE(failures.size(), 1);
        server.emptyAtBoundary = false;
        source.requestRange(first, first + 9, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(source.itemCount(), 732);
        QVERIFY(source.isAvailable(first + 9));

        // Also protect mappings that have already been published to the view.
        // A failed body rehydration must not remove their logical rows/widgets.
        server.emptyAtBoundary = true;
        for (int i = first; i <= first + 9; ++i) QVERIFY(channel.evictPostBody(id(i)));
        QSignalSpy countChanges(&source, &AbstractPostSource::itemCountChanged);
        source.requestRange(first, first + 9, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(failures.size(), 2);
        QCOMPARE(countChanges.size(), 0);
        QCOMPARE(source.itemCount(), 732);
        for (int i = first; i <= first + 9; ++i) QCOMPARE(source.indexOfPost(id(i)), i);
    }
    void boundedNetworkCost_data()
    {
        QTest::addColumn<int>("target");
        QTest::addColumn<bool>("knownSuffix");
        QTest::addColumn<int>("requestCount");
        QTest::addColumn<int>("replyCount");
        QTest::newRow("cold-382") << 382 << false << 1 << 30;
        QTest::newRow("cold-158") << 158 << false << 1 << 30;
        QTest::newRow("cold-251") << 251 << false << 1 << 30;
        QTest::newRow("suffix-436-to-382") << 382 << true << 1 << 30;
    }
    void boundedNetworkCost()
    {
        QFETCH(int, target);
        QFETCH(bool, knownSuffix);
        QFETCH(int, requestCount);
        QFETCH(int, replyCount);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        if (knownSuffix) {
            source.requestRange(436, 731, AbstractPostSource::RequestReason::Seek, 1);
            QTRY_COMPARE(finished.size(), 1);
            finished.clear();
            server.requests.clear();
            server.returnedReplies = 0;
            server.responseBytes = 0;
        }
        QSignalSpy resolved(&source, &AbstractPostSource::seekTargetResolved);
        source.requestRange(target, target + 9, AbstractPostSource::RequestReason::Seek, 2);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(resolved.size(), 1);
        const int placed = resolved.first().first().toInt();
        for (int i = placed - 4; i <= placed + 5; ++i) {
            QVERIFY(source.postAt(i));
            QCOMPARE(source.isPostPositionAuthoritative(source.postAt(i)->id), knownSuffix);
        }
        QCOMPARE(server.requests.size(), requestCount);
        QCOMPARE(server.returnedReplies, replyCount);
        QVERIFY(server.largestRequestedPage <= 50);
        QVERIFY(server.responseBytes > 0);
        QVERIFY(server.responseBytes <= replyCount * 400 + requestCount * 1000);
    }
    void ordinaryDemandSharesInFlightPage_data()
    {
        QTest::addColumn<int>("secondFirst");
        QTest::addColumn<int>("secondLast");
        QTest::addColumn<int>("expectedRequests");
        QTest::addColumn<int>("expectedReplies");
        QTest::newRow("adjacent") << 111 << 115 << 2 << 21;
        QTest::newRow("overlapping") << 104 << 115 << 2 << 21;
        QTest::newRow("contained") << 104 << 108 << 1 << 11;
    }
    void ordinaryDemandSharesInFlightPage()
    {
        QFETCH(int, secondFirst);
        QFETCH(int, secondLast);
        QFETCH(int, expectedRequests);
        QFETCH(int, expectedReplies);
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        source.requestRange(1, 99, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 1);
        finished.clear();
        server.requests.clear();
        server.returnedReplies = 0;
        server.responseBytes = 0;
        server.responseDelayMs = 150;
        source.requestRange(100, 110, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(server.requests.size(), 1);
        source.requestRange(secondFirst, secondLast, AbstractPostSource::RequestReason::Scroll, 0);
        QTest::qWait(30);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(finished.size(), 0);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(finished.at(0).at(0).toInt(), 100);
        QCOMPARE(finished.at(0).at(1).toInt(), 110);
        QCOMPARE(finished.at(1).at(0).toInt(), secondFirst);
        QCOMPARE(finished.at(1).at(1).toInt(), secondLast);
        for (int i = 100; i <= std::max(110, secondLast); ++i) QVERIFY(source.isAvailable(i));
        QCOMPARE(server.requests.size(), expectedRequests);
        QCOMPARE(server.returnedReplies, expectedReplies);
        QVERIFY(server.responseBytes <= expectedReplies * 400 + expectedRequests * 1000);
    }
    void seekDoesNotWaitForMergedScroll()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        source.requestRange(1, 99, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 1);
        finished.clear();
        server.requests.clear();
        server.responseDelayMs = 200;
        source.requestRange(100, 110, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(server.requests.size(), 1);
        source.requestRange(111, 115, AbstractPostSource::RequestReason::Scroll, 0);
        QSignalSpy resolved(&source, &AbstractPostSource::seekTargetResolved);
        source.requestRange(650, 659, AbstractPostSource::RequestReason::Seek, 1);
        QTRY_COMPARE(server.requests.size(), 2);
        QCOMPARE(finished.size(), 0);
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(server.requests.size(), 2); // one stale scroll + one timestamp page
        QCOMPARE(source.indexOfPost(id(100)), -1); // stale page was not placed
        QCOMPARE(resolved.size(), 1);
        QVERIFY(source.isAvailable(resolved.first().first().toInt()));
    }
    void evictionOwnsIdAcrossBodyDestruction()
    {
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        BackendPost* reply = channel.addPost(post(1));
        QSignalSpy availability(&channel, &BackendChannel::onPostBodyAvailabilityChanged);
        QVERIFY(channel.evictPostBody(reply->id));
        QCOMPARE(availability.size(), 1);
        QCOMPARE(availability.first().first().toString(), id(1));
        QCOMPARE(availability.first().at(1).toBool(), false);
    }
    void widgetShowsColdPermalinkWithoutOrdinalTraversal()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.replyCount = 10000;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        QJsonObject root = post(0);
        root.insert("reply_count", 10000);
        root.insert("last_reply_at", timestamp(10000));
        channel.addPost(root);
        channel.mergePostContext(QJsonArray {id(5000)}, QJsonObject {{id(5000), post(5000)}});
        ThreadPostSource source(backend, channel, id(0));
        source.ensurePostIndex(id(5000));
        SourceList list(source, true);
        list.resize(480, 360);
        list.followPost(id(5000));
        list.show();
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        QTRY_VERIFY(list.itemWidget(source.indexOfPost(id(5000))));
        QTRY_VERIFY([&] {
            const auto visible = list.visibleRange();
            for (int i = visible.first; i <= visible.last; ++i) if (!list.itemWidget(i)) return false;
            return visible.isValid();
        }());
        QCOMPARE(failures.size(), 0);
        QVERIFY(!source.isPostPositionAuthoritative(id(5000)));
        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.returnedReplies, 30);
        QTest::qWait(100);
        QCOMPARE(server.requests.size(), 2);
    }
    void coldScrollbarSeekIsBoundedInLargeThread()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.replyCount = 10000;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        auto root = post(0);
        root.insert("reply_count", server.replyCount);
        root.insert("last_reply_at", timestamp(server.replyCount));
        channel.addPost(root);
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy resolved(&source, &AbstractPostSource::seekTargetResolved);
        source.requestRange(4995, 5004, AbstractPostSource::RequestReason::Seek, 1);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(resolved.size(), 1);
        QCOMPARE(server.requests.size(), 1);
        QCOMPARE(server.returnedReplies, 30);
        QVERIFY(server.responseBytes < 15000);
        QCOMPARE(server.largestRequestedPage, 30);
        const int center = resolved.first().first().toInt();
        const QString centerId = source.postAt(center)->id;
        QVERIFY(centerId != id(center)); // strongly nonuniform timestamp fixture
        QVERIFY(!source.isPostPositionAuthoritative(centerId));
        QVERIFY(!source.postAt(center - 16));
        QVERIFY(!source.postAt(center + 15));
        QVERIFY(channel.evictPostBody(source.postAt(center - 15)->id));
        source.requestRange(center - 25, center - 16, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(server.requests.size(), 2);
        QCOMPARE(server.returnedReplies, 40);
        QCOMPARE(source.indexOfPost(centerId), center);
        QVERIFY(source.isAvailable(center - 25));
        QVERIFY(!source.postAt(center - 26));
    }

    void islandsMergeAndPromoteWithoutMovingViewport()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy failed(&source, &ThreadPostSource::rangeRequestFailed);
        source.requestRange(295, 304, AbstractPostSource::RequestReason::Seek, 1);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(source.postAt(299));
        const QString followed = source.postAt(299)->id;
        const int actual = followed.mid(4).toInt();
        // Keep a second, unrelated island. It must survive both the subsequent
        // identity merge and promotion of the first island.
        source.requestRange(95, 104, AbstractPostSource::RequestReason::Seek, 2);
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(source.postAt(99));
        const QString other = source.postAt(99)->id;
        const int otherIndex = source.indexOfPost(other);

        SourceList list(source, true);
        list.resize(480, 360);
        list.scrollToIndex(source.indexOfPost(followed), LongListWidget::Alignment::Center);
        list.show();
        QTRY_VERIFY(list.itemWidget(source.indexOfPost(followed)));
        QTest::qWait(100);
        QPointer<QWidget> widget = list.itemWidget(source.indexOfPost(followed));
        const int originalY = widget->y();
        const int originalBar = list.verticalScrollBar()->value();
        // A separate timestamp page overlaps IDs but is numerically detached.
        const int beforeMerge = finished.size();
        source.requestRange(335, 344, AbstractPostSource::RequestReason::Seek, 100);
        QTRY_VERIFY(finished.size() > beforeMerge);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(source.indexOfPost(other), otherIndex);
        QVERIFY(widget);
        QCOMPARE(list.itemWidget(source.indexOfPost(followed)), widget.data());
        QCOMPARE(widget->y(), originalY);
        QVERIFY(!source.isPostPositionAuthoritative(followed));
        // Walk an exact suffix into the island: shared identity proves rank.
        const int beforePromotion = finished.size();
        source.requestRange(actual - 20, 731, AbstractPostSource::RequestReason::Scroll, 101);
        QTRY_VERIFY(finished.size() > beforePromotion);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(source.indexOfPost(followed), actual);
        QVERIFY(source.isPostPositionAuthoritative(followed));
        QCOMPARE(source.indexOfPost(other), otherIndex);
        QVERIFY(!source.isPostPositionAuthoritative(other));
        QVERIFY(widget);
        QCOMPARE(list.itemWidget(actual), widget.data());
        QCOMPARE(widget->y(), originalY);
        QVERIFY(list.verticalScrollBar()->value() != originalBar);
        // Every transition of authority between mapped rows has an empty slot.
        for (int i = 1; i < source.itemCount(); ++i) {
            const auto* left = source.postAt(i - 1);
            const auto* right = source.postAt(i);
            if (left && right) QCOMPARE(source.isPostPositionAuthoritative(left->id),
                                        source.isPostPositionAuthoritative(right->id));
        }
    }
    void widgetRandomSeeksMaterializeCorrectPosts()
    {
        ThreadServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        server.pageLimit = 13;
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(backend.getStorage(), QJsonObject {{"id", "channel"}, {"type", "O"}});
        channel.addPost(post(0));
        ThreadPostSource source(backend, channel, id(0));
        SourceList list(source);
        list.resize(480, 360);
        list.show();
        QSignalSpy failures(&source, &ThreadPostSource::rangeRequestFailed);
        for (int target : {382, 158, 650, 251, 362, 50, 700, 382}) {
            qInfo() << "Seeking" << target;
            auto* bar = list.verticalScrollBar();
            bar->setSliderDown(true);
            const int value = int(double(target) / source.itemCount() * list.contentHeight());
            bar->setValue(value);
            QVERIFY(QMetaObject::invokeMethod(bar, "sliderMoved", Qt::DirectConnection, Q_ARG(int, value)));
            bar->setSliderDown(false);
            QTRY_VERIFY_WITH_TIMEOUT(list.visibleRange().isValid() && list.materializedCount() > 0, 5000);
            const bool materialized = QTest::qWaitFor([&] {
                const auto visible = list.visibleRange();
                for (int i = visible.first; i <= visible.last; ++i) {
                    const auto* label = qobject_cast<QLabel*>(list.itemWidget(i));
                    if (!label || !source.postAt(i) || label->text() != source.postAt(i)->id) return false;
                }
                return true;
            }, 5000);
            if (!materialized) {
                const auto visible = list.visibleRange();
                qWarning() << "Viewport" << visible.first << visible.last << "widgets" << list.materializedIndices();
                for (int i = visible.first; i <= visible.last; ++i) {
                    auto* label = qobject_cast<QLabel*>(list.itemWidget(i));
                    qWarning() << i << source.isAvailable(i) << list.isItemAvailable(i)
                               << (label ? label->text() : QStringLiteral("no widget"));
                }
            }
            QVERIFY(materialized);
        }
        QCOMPARE(failures.size(), 0);
        const int requests = server.requests.size();
        QTest::qWait(100);
        QCOMPARE(server.requests.size(), requests);
        QVERIFY(requests < 160);
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir state;
    qputenv("XDG_CACHE_HOME", state.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", state.path().toUtf8());
    qputenv("XDG_DATA_HOME", state.path().toUtf8());
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false\n") + QString::fromUtf8(qgetenv("QT_LOGGING_RULES")));
    ThreadPostSourceIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ThreadPostSourceIntegrationTest.moc"
