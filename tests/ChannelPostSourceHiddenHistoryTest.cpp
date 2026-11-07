#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/PendingPostService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChannelPostSource.h"
#include "chat-area/FilteredPostSource.h"
#include "chat-area/OutboxPostSource.h"
#include "chat-area/PostFilterPolicy.h"
#include "widgets/LongListWidget.h"

using namespace Mattermost;

namespace {

QString postId(int index)
{
    return QStringLiteral("post%1").arg(index, 22, 10, QLatin1Char('0'));
}

qint64 timestamp(int index)
{
    return 1000000000000LL + index * 1000LL;
}

QJsonObject rootPost(int index, int ordinaryCount, bool interleaved = false)
{
    // The mixed fixture has 59 ordinary prefix rows and an ordinary row at
    // every multiple of ten thereafter: 113 counted roots among 600 raw rows.
    const bool hidden = interleaved ? index >= 59 && index % 10 != 0
                                    : index >= ordinaryCount;
    return {
        {QStringLiteral("id"), postId(index)},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("user_id"), QStringLiteral("other")},
        {QStringLiteral("message"),
         hidden ? QStringLiteral("other joined the channel.")
                : QStringLiteral("real message %1").arg(index)},
        {QStringLiteral("type"),
         hidden ? (index % 2 ? QStringLiteral("system_join_channel")
                             : QStringLiteral("system_leave_channel")) : QString()},
        {QStringLiteral("props"), QJsonObject {
            {QStringLiteral("username"), QStringLiteral("other")}
        }},
        {QStringLiteral("create_at"), double(timestamp(index))},
        {QStringLiteral("update_at"), double(timestamp(index))}
    };
}

class HiddenHistoryServer final : public QTcpServer
{
public:
    int ordinaryCount = 19;
    int hiddenCount = 16;
    int requests = 0;
    QStringList requestUrls;
    bool failNextCursor = false;
    int viewportResponseDelayMs = 0;
    bool interleaved = false;
    QHash<int, QJsonObject> postOverrides;

    HiddenHistoryServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (socket->property("answered").toBool()) {
                        return;
                    }
                    const QByteArray received =
                        socket->property("incoming").toByteArray() + socket->readAll();
                    socket->setProperty("incoming", received);
                    if (!received.contains("\r\n\r\n")) {
                        return;
                    }
                    socket->setProperty("answered", true);
                    ++requests;

                    const QUrl url(QString::fromUtf8(received.split(' ').value(1)));
                    requestUrls.push_back(url.toString());
                    const QJsonObject payload = postsResponse(QUrlQuery(url));
                    const QByteArray body =
                        QJsonDocument(payload).toJson(QJsonDocument::Compact);
                    const bool failed = failNextCursor
                        && QUrlQuery(url).hasQueryItem(QStringLiteral("before"));
                    if (failed) failNextCursor = false;
                    const QByteArray response =
                        (failed ? QByteArrayLiteral("HTTP/1.1 500 Internal Server Error\r\n")
                                : QByteArrayLiteral("HTTP/1.1 200 OK\r\n"))
                        + QByteArrayLiteral("Content-Type: application/json\r\n")
                        + QByteArrayLiteral("Connection: close\r\n")
                        + QByteArrayLiteral("Content-Length: ")
                        + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\n\r\n") + body;
                    const int delay = QUrlQuery(url).queryItemValue("per_page") == "11"
                        ? viewportResponseDelayMs : 0;
                    QTimer::singleShot(delay, socket, [socket, response] {
                        socket->write(response);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }

    // Mirrors getPostsAround()/AddCursorIdsForPostList() in Mattermost:
    // before walks backwards, after selects the nearest newer posts, and both
    // serialize order newest first. Cursors are exclusive; page offsets apply
    // within the selected side of the anchor.
    QJsonObject postsResponse(const QUrlQuery& query) const
    {
        const int page = qMax(0, query.queryItemValue(QStringLiteral("page")).toInt());
        const int perPage = query.hasQueryItem(QStringLiteral("per_page"))
            ? qBound(1, query.queryItemValue(QStringLiteral("per_page")).toInt(), 200)
            : 60;
        const int total = ordinaryCount + hiddenCount;
        const QString before = query.queryItemValue(QStringLiteral("before"));
        const QString after = query.queryItemValue(QStringLiteral("after"));
        const QString anchor = after.isEmpty() ? before : after;
        int anchorIndex = -1;
        for (int i = 0; i < total; ++i) {
            if (postId(i) == anchor) {
                anchorIndex = i;
                break;
            }
        }

        int first = qMax(0, total - (page + 1) * perPage);
        int last = total - 1 - page * perPage;
        if (!anchor.isEmpty()) {
            if (anchorIndex < 0) {
                first = 0;
                last = -1;
            } else if (!after.isEmpty()) {
                first = anchorIndex + 1 + page * perPage;
                last = qMin(total - 1, first + perPage - 1);
            } else {
                last = anchorIndex - 1 - page * perPage;
                first = qMax(0, last - perPage + 1);
            }
        }

        QJsonObject posts;
        QJsonArray order;
        for (int index = last; index >= first; --index) {
            order.append(postId(index));
            posts.insert(postId(index), postOverrides.value(
                index, rootPost(index, ordinaryCount, interleaved)));
        }
        QString prev = !order.isEmpty() && first > 0 ? postId(first - 1) : QString();
        QString next = !order.isEmpty() && last < total - 1 ? postId(last + 1) : QString();
        if (!after.isEmpty() && page == 0) {
            prev = after;
        } else if (!before.isEmpty() && page == 0) {
            next = before;
        }
        return {
            {QStringLiteral("order"), order},
            {QStringLiteral("posts"), posts},
            {QStringLiteral("prev_post_id"), prev},
            {QStringLiteral("next_post_id"), next}
        };
    }

    bool start()
    {
        if (!listen(QHostAddress::LocalHost)) {
            return false;
        }
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(serverPort()));
        return true;
    }
};

QJsonObject channelJson(int ordinaryCount, int hiddenCount)
{
    return {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("name"), QStringLiteral("engineering")},
        {QStringLiteral("display_name"), QStringLiteral("Engineering")},
        {QStringLiteral("type"), QStringLiteral("O")},
        // Mattermost excludes routine membership posts from these counters.
        {QStringLiteral("total_msg_count"), ordinaryCount},
        {QStringLiteral("total_msg_count_root"), ordinaryCount},
        {QStringLiteral("last_post_at"),
         double(timestamp(ordinaryCount + hiddenCount - 1))},
        {QStringLiteral("last_root_post_at"),
         double(timestamp(ordinaryCount + hiddenCount - 1))}
    };
}

// Use the real list's demand/availability lifecycle, with cheap fixed-height
// rows. This is the same source signal wiring as ChatLogWidget, including the
// completion reconciliation for a range already resident by request time.
class SourceList final : public LongListWidget
{
public:
    explicit SourceList(AbstractPostSource& source) : source(source)
    {
        setDefaultItemHeight(40);
        setPrefetchScreens(0);
        setItemCount(source.itemCount());
        for (int index = 0; index < source.itemCount(); ++index) {
            if (source.isAvailable(index)) setRangeAvailable(index, index);
        }
        connect(this, &LongListWidget::rangeRequested, &source,
                [&source](int first, int last, RequestReason reason, quint64 generation) {
            source.requestRange(first, last,
                static_cast<AbstractPostSource::RequestReason>(reason), generation);
        });
        connect(&source, &AbstractPostSource::itemsInserted, this,
                [this](int first, int count) { insertItems(first, count); });
        connect(&source, &AbstractPostSource::itemsRemoved, this, &LongListWidget::removeItems);
        connect(&source, &AbstractPostSource::itemCountChanged, this, &LongListWidget::setItemCount);
        connect(&source, &AbstractPostSource::layoutChanged, this, [this](int first, int last) {
            reconcileItemLayout(first, last, this->source.itemCount());
        });
        connect(&source, &AbstractPostSource::bodyAvailabilityChanged, this,
                [this](int first, int last, bool available) {
            setRangeAvailable(first, last, available);
        });
        connect(&source, &AbstractPostSource::rangeAvailable, this, [this](int first, int last) {
            for (int index = first; index <= last; ++index) {
                setRangeAvailable(index, index, this->source.isAvailable(index));
            }
        });
        connect(&source, &AbstractPostSource::rangeRequestFinished, this, [this](int first, int last) {
            for (int index = qMax(0, first); index <= qMin(last, this->source.itemCount() - 1); ++index) {
                if (this->source.isAvailable(index)) setRangeAvailable(index, index);
            }
            finishRangeRequest(first, last);
        });
    }

    QWidget* widgetFor(int index) const { return itemWidget(index); }
    int created = 0;
    int destroyed = 0;

protected:
    void destroyItemWidget(int index, QWidget* widget) override
    {
        ++destroyed;
        LongListWidget::destroyItemWidget(index, widget);
    }

    QWidget* createItemWidget(int index) override
    {
        const BackendPost* post = source.postAt(index);
        if (!post) return nullptr;
        ++created;
        auto* label = new QLabel(post->id);
        label->setFixedHeight(40);
        return label;
    }
    QString itemIdentity(const QWidget* widget) const override
    {
        const auto* label = qobject_cast<const QLabel*>(widget);
        return label ? label->text() : QString();
    }
    int indexOfItemIdentity(const QString& id) const override { return source.indexOfPost(id); }
    bool isModelItemAvailable(int index) const override { return source.isAvailable(index); }

private:
    AbstractPostSource& source;
};

bool mainTimelinePost(const BackendPost& post)
{
    return shouldShowMainTimelinePost(post.type, post.props.toObject(), QStringLiteral("self"));
}

} // namespace

class ChannelPostSourceHiddenHistoryTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void mixedNavigationStopsLoadingWhenViewportIsSatisfied()
    {
        HiddenHistoryServer server;
        server.ordinaryCount = 113;
        server.hiddenCount = 487;
        server.interleaved = true;
        QVERIFY(server.start());
        Backend backend;
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")}
        }, true);
        BackendChannel channel(backend.getStorage(), channelJson(113, 487));
        QJsonArray ids;
        QJsonObject posts;
        QStringList context;
        for (int index = 300; index <= 330; ++index) {
            ids.append(postId(index));
            posts.insert(postId(index), rootPost(index, 113, true));
            context.push_back(postId(index));
        }
        channel.mergePostContext(ids, posts);
        ChannelPostSource raw(backend, channel);
        QSignalSpy completed(&raw, &AbstractPostSource::rangeRequestFinished);
        raw.requestRange(108, 112, AbstractPostSource::RequestReason::Initial, 0);
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 5000);
        QVERIFY(raw.adoptNavigationContext(postId(320), context, false, false));

        FilteredPostSource visible(raw, mainTimelinePost);
        OutboxPostSource presentation(visible, PendingPostService::instance(backend),
                                     channel.id, QString());
        SourceList list(presentation);
        QSignalSpy demands(&list, &LongListWidget::rangeRequested);
        QSignalSpy finished(&presentation, &AbstractPostSource::rangeRequestFinished);
        list.resize(500, 240);
        list.show();
        list.scrollToIndex(presentation.indexOfPost(postId(320)), LongListWidget::Alignment::Center);
        QVERIFY(list.lockViewportToItem(presentation.indexOfPost(postId(320)),
                                        LongListWidget::Alignment::Center, 0));
        const auto settled = [&] {
            if (demands.size() != finished.size()) return false;
            const auto range = list.visibleRange();
            if (!range.isValid()) return false;
            for (int index = range.first; index <= range.last; ++index) {
                if (!presentation.isAvailable(index) || !list.widgetFor(index)) return false;
            }
            const int requests = server.requests;
            QTest::qWait(100);
            return server.requests == requests && demands.size() == finished.size();
        };
        QTRY_VERIFY_WITH_TIMEOUT(settled(), 5000);
        qInfo() << "Mixed navigation requests/widgets/destroyed:"
                << server.requests << list.created << list.destroyed;
        // Each ten-row cursor page adds only one ordinary row here. Six
        // viewport rows plus the normal neighbour margin need at most twelve
        // requests, independent of the distant newest page at raw row 590.
        QVERIFY2(server.requests <= 12, "Navigation must not drain history outside the viewport/prefetch window");
        QCOMPARE(list.destroyed, 0);
        QVERIFY(list.created <= 16);
        QVERIFY(list.hasViewportLock());
        QVERIFY(list.widgetFor(presentation.indexOfPost(postId(320))));
    }

    void reopeningSourceRehydratesEvictedTail()
    {
        HiddenHistoryServer server;
        server.ordinaryCount = 124;
        server.hiddenCount = 0;
        QVERIFY(server.start());
        Backend backend;
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")}
        }, true);
        BackendChannel channel(backend.getStorage(), channelJson(124, 0));
        ChannelPostSource raw(backend, channel);
        FilteredPostSource visible(raw, mainTimelinePost);
        OutboxPostSource presentation(visible, PendingPostService::instance(backend),
                                     channel.id, QString());
        {
            SourceList first(presentation);
            first.resize(500, 240);
            first.show();
            first.scrollToEnd();
            QTRY_VERIFY_WITH_TIMEOUT(first.widgetFor(presentation.indexOfPost(postId(123))), 5000);
        }
        const int tail = raw.indexOfPost(postId(123));
        QVERIFY(tail >= 0);
        QVERIFY(channel.evictPostBody(postId(123)));
        QVERIFY(!raw.isAvailable(tail));
        const int previousRequests = server.requests;
        SourceList reopened(presentation);
        reopened.resize(500, 240);
        reopened.show();
        reopened.scrollToEnd();
        QTRY_VERIFY_WITH_TIMEOUT(reopened.widgetFor(presentation.indexOfPost(postId(123))), 5000);
        QVERIFY(raw.isAvailable(raw.indexOfPost(postId(123))));
        QVERIFY(server.requests > previousRequests);
    }

    void navigationExtensionCanOutgrowTheEntireEstimatedSource()
    {
        HiddenHistoryServer server;
        server.ordinaryCount = 22;
        server.hiddenCount = 58;
        // The last ordinary message is in the very cursor page which makes
        // the accumulated navigation context larger than the entire estimate.
        server.postOverrides.insert(44, rootPost(44, 80));
        QVERIFY(server.start());
        Backend backend;
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")}
        }, true);
        BackendChannel channel(backend.getStorage(), channelJson(40, 40));
        QJsonArray order;
        QJsonObject posts;
        QStringList context;
        for (int index = 10; index <= 40; ++index) {
            order.append(postId(index));
            posts.insert(postId(index), rootPost(index, 22));
            context.push_back(postId(index));
        }
        channel.mergePostContext(order, posts);
        ChannelPostSource raw(backend, channel);
        // Unread can point to a filtered membership post, just as in the
        // uploaded trace. Page-zero validation has no shared identity with
        // this context and therefore cannot publish its independent tail yet.
        QVERIFY(raw.adoptNavigationContext(postId(25), context, false, false));
        const int guardIndex = raw.indexOfPost(postId(40)) + 1;
        QSignalSpy finished(&raw, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy inserted(&raw, &AbstractPostSource::itemsInserted);
        raw.requestRange(guardIndex, guardIndex, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(channel.postIdToPost.contains(postId(44)));
        QVERIFY2(raw.indexOfPost(postId(44)) >= 0,
                 "A successfully fetched post must not be dropped when contextCount exceeds itemCount");
        QCOMPARE(raw.itemCount(), 43); // 41 connected identities + two guards.
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(inserted.at(0).at(0).toInt(), 40);
        QCOMPARE(inserted.at(0).at(1).toInt(), 3);
        QVERIFY(raw.isAvailable(raw.indexOfPost(postId(44))));
        QVERIFY(!raw.isPostPositionAuthoritative(postId(44)));

        FilteredPostSource visible(raw, mainTimelinePost);
        OutboxPostSource presentation(visible, PendingPostService::instance(backend),
                                     channel.id, QString());
        SourceList list(presentation);
        list.resize(500, 240);
        list.show();
        list.scrollToEnd();
        const auto tailMaterialized = [&] {
            const auto range = list.visibleRange();
            if (!range.isValid() || range.last != presentation.itemCount() - 1
                || presentation.postIdAt(range.last) != postId(44)) return false;
            for (int index = range.first; index <= range.last; ++index) {
                if (!presentation.isAvailable(index) || !list.widgetFor(index)) return false;
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(tailMaterialized(), 5000);
        // Reaching newest does not prove oldest. Keep a requestable prefix
        // instead of silently making post10 look like the start of history.
        QVERIFY(raw.postIdAt(0).isEmpty());
        QVERIFY(!raw.isAvailable(0));
        QCOMPARE(list.destroyed, 0);
        QVERIFY(server.requests <= 8);
    }

    void navigationExtensionPublishesOnlyNewSlots()
    {
        HiddenHistoryServer server;
        server.ordinaryCount = 100;
        server.hiddenCount = 100;
        QVERIFY(server.start());
        Backend backend;
        auto metadata = channelJson(100, 100);
        metadata.insert(QStringLiteral("create_at"), double(timestamp(0)));
        BackendChannel channel(backend.getStorage(), metadata);
        QJsonArray order;
        QJsonObject posts;
        QStringList context;
        for (int index = 40; index <= 70; ++index) {
            order.append(postId(index));
            posts.insert(postId(index), rootPost(index, 100));
            context.push_back(postId(index));
        }
        channel.mergePostContext(order, posts);
        ChannelPostSource raw(backend, channel);
        QSignalSpy finished(&raw, &AbstractPostSource::rangeRequestFinished);
        raw.requestRange(95, 99, AbstractPostSource::RequestReason::Initial, 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QSignalSpy remapped(&raw, &AbstractPostSource::layoutChanged);
        QVERIFY(raw.adoptNavigationContext(postId(55), context, false, false));
        QCOMPARE(remapped.size(), 0);
        const int targetIndex = raw.indexOfPost(postId(55));
        const int extensionFirst = raw.indexOfPost(postId(70)) + 1;
        QSignalSpy available(&raw, &AbstractPostSource::rangeAvailable);
        raw.requestRange(extensionFirst, extensionFirst, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
        QCOMPARE(raw.indexOfPost(postId(55)), targetIndex);
        QCOMPARE(remapped.size(), 0);
        QCOMPARE(available.size(), 1);
        QCOMPARE(available.at(0).at(0).toInt(), extensionFirst);
        QCOMPARE(available.at(0).at(1).toInt(), extensionFirst + 9);
    }

    void httpPagination_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<int>("first");
        QTest::addColumn<int>("last");
        QTest::addColumn<int>("prev");
        QTest::addColumn<int>("next");
        QTest::newRow("newest") << "page=0&per_page=10" << 25 << 34 << 24 << -1;
        QTest::newRow("absolute") << "page=2&per_page=10" << 5 << 14 << 4 << 15;
        QTest::newRow("short") << "page=3&per_page=10" << 0 << 4 << -1 << 5;
        QTest::newRow("empty") << "page=4&per_page=10" << 0 << -1 << -1 << -1;
        QTest::newRow("before") << "before=" + postId(24) + "&per_page=10"
                                 << 14 << 23 << 13 << 24;
        QTest::newRow("before-page") << "before=" + postId(24) + "&page=1&per_page=10"
                                      << 4 << 13 << 3 << 14;
        QTest::newRow("before-oldest") << "before=" + postId(0) + "&per_page=10"
                                        << 0 << -1 << -1 << 0;
        QTest::newRow("after") << "after=" + postId(9) + "&per_page=10"
                                << 10 << 19 << 9 << 20;
        QTest::newRow("after-page") << "after=" + postId(9) + "&page=1&per_page=10"
                                     << 20 << 29 << 19 << 30;
        QTest::newRow("after-short") << "after=" + postId(29) + "&per_page=10"
                                      << 30 << 34 << 29 << -1;
        QTest::newRow("after-newest") << "after=" + postId(34) + "&per_page=10"
                                       << 0 << -1 << 34 << -1;
    }

    void httpPagination()
    {
        QFETCH(QString, query);
        QFETCH(int, first);
        QFETCH(int, last);
        QFETCH(int, prev);
        QFETCH(int, next);
        HiddenHistoryServer server;
        const QJsonObject response = server.postsResponse(QUrlQuery(query));
        QJsonArray expected;
        for (int index = last; index >= first; --index) {
            expected.append(postId(index));
        }
        QCOMPARE(response.value("order").toArray(), expected);
        const QJsonObject posts = response.value("posts").toObject();
        QCOMPARE(posts.size(), expected.size());
        for (const auto& id : expected) {
            QCOMPARE(posts.value(id.toString()).toObject().value("id"), id);
        }
        QCOMPARE(response.value("prev_post_id").toString(), prev < 0 ? QString() : postId(prev));
        QCOMPARE(response.value("next_post_id").toString(), next < 0 ? QString() : postId(next));
    }

    void paginationAfterFilteringExhaustsTheViewport_data()
    {
        QTest::addColumn<int>("ordinaryCount");
        QTest::addColumn<int>("hiddenCount");
        QTest::newRow("nineteen-rows") << 19 << 16;
        QTest::newRow("estimated-prefix-drift") << 21 << 19;
    }

    void paginationAfterFilteringExhaustsTheViewport()
    {
        QFETCH(int, ordinaryCount);
        QFETCH(int, hiddenCount);
        HiddenHistoryServer server;
        server.ordinaryCount = ordinaryCount;
        server.hiddenCount = hiddenCount;
        // Force the background ten-row cache validation to arrive first.
        server.viewportResponseDelayMs = 50;
        QVERIFY(server.start());
        Backend backend;
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")}
        }, true);
        BackendChannel channel(backend.getStorage(), channelJson(ordinaryCount, hiddenCount));
        ChannelPostSource raw(backend, channel);
        FilteredPostSource visible(raw, mainTimelinePost);
        QSignalSpy finished(&visible, &AbstractPostSource::rangeRequestFinished);

        bool requestedOlder = false;
        // Issue [0,7] synchronously from the viewport availability notification,
        // while [8,18]'s gate is still active. A queued second request would
        // miss the original uncovered-adjacent-attachment regression.
        connect(&raw, &AbstractPostSource::rangeAvailable, &visible, [&](int first, int last) {
            // Startup cache validation can publish a ten-row suffix first.
            // Reenter only for the eleven-row viewport response under test.
            if (requestedOlder || first != ordinaryCount - 11 || last != ordinaryCount - 1) return;
            requestedOlder = true;
            QCOMPARE(visible.itemCount(), ordinaryCount - 11);
            visible.requestRange(0, visible.itemCount() - 1,
                                 AbstractPostSource::RequestReason::Scroll, 0);
        });
        visible.requestRange(ordinaryCount - 11, ordinaryCount - 1,
                             AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(requestedOlder);
        const int firstReturned = ordinaryCount + hiddenCount - 21;
        // The cursor response has a predecessor outside the count estimate.
        // That row must remain unloaded and demandable after completion.
        QCOMPARE(raw.indexOfPost(postId(firstReturned)), 1);
        QVERIFY(!raw.isAvailable(0));
        QVERIFY(!visible.isAvailable(0));
        QVERIFY(visible.itemCount() > 1);
        bool sawCursor = false;
        for (const QString& url : server.requestUrls) {
            const QUrlQuery query {QUrl(url)};
            if (query.queryItemValue("before") == postId(ordinaryCount + hiddenCount - 11)) {
                sawCursor = true;
                QCOMPARE(query.queryItemValue("per_page").toInt(), 10);
            }
        }
        QVERIFY(sawCursor);
    }

    void predecessorDemandDuringPrependIsNotAttachedToOldCoordinates()
    {
        HiddenHistoryServer server;
        server.hiddenCount = 40;
        QVERIFY(server.start());
        Backend backend;
        BackendChannel channel(backend.getStorage(), channelJson(19, 40));
        ChannelPostSource raw(backend, channel);
        FilteredPostSource visible(raw, mainTimelinePost);
        QSignalSpy finished(&visible, &AbstractPostSource::rangeRequestFinished);
        bool requestedPredecessor = false;
        connect(&raw, &AbstractPostSource::rangeAvailable, &visible, [&](int first, int last) {
            if (requestedPredecessor || last - first + 1 != 19) return;
            requestedPredecessor = true;
            QCOMPARE(visible.itemCount(), 1);
            QVERIFY(!visible.isAvailable(0));
            visible.requestRange(0, 0, AbstractPostSource::RequestReason::Scroll, 0);
        });
        visible.requestRange(0, 18, AbstractPostSource::RequestReason::Initial, 0);
        QTRY_COMPARE(finished.size(), 2);
        // The new prefix was never part of the original [0,18] HTTP response.
        // It must start a real cursor request even while that gate is active.
        QCOMPARE(raw.indexOfPost(postId(30)), 1);
        QVERIFY(!raw.isAvailable(0));
    }

    void failedPredecessorRequestStaysUnavailableAndCanRetry()
    {
        HiddenHistoryServer server;
        server.hiddenCount = 40;
        QVERIFY(server.start());
        Backend backend;
        BackendChannel channel(backend.getStorage(), channelJson(19, 40));
        ChannelPostSource raw(backend, channel);
        FilteredPostSource visible(raw, mainTimelinePost);
        QSignalSpy finished(&visible, &AbstractPostSource::rangeRequestFinished);
        visible.requestRange(0, 18, AbstractPostSource::RequestReason::Initial, 0);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(visible.itemCount(), 1);
        server.failNextCursor = true;
        visible.requestRange(0, 0, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(!visible.isAvailable(0));
        const int failedRequests = server.requests;
        QTest::qWait(100);
        QCOMPARE(server.requests, failedRequests);
        visible.requestRange(0, 0, AbstractPostSource::RequestReason::Scroll, 0);
        QTRY_COMPARE(finished.size(), 3);
        QCOMPARE(raw.indexOfPost(postId(30)), 1);
        QVERIFY(!visible.isAvailable(0));
    }

    void navigationContextReservesRowsMissingFromRootCounter()
    {
        // The real endpoint contains 80 roots, 58 of them count-excluded
        // join/leave events. The initial metadata reserves just 40 slots.
        // An exact 10-post newest page occupies the tail, leaving a free
        // prefix of 30 slots, too short for a 31-post navigation context
        // plus its two unknown-boundary guard slots.
        HiddenHistoryServer server;
        server.ordinaryCount = 22;
        server.hiddenCount = 58;
        QVERIFY(server.start());

        Backend backend;
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")}
        }, true);
        BackendChannel channel(backend.getStorage(), channelJson(40, 0));

        // The context is resident, but is not a verified newest cache tail.
        // Seed it before constructing the source so it cannot acquire guessed
        // authoritative indices from live-post insertion signals.
        QJsonArray ids;
        QJsonObject posts;
        QStringList context;
        for (int i = 0; i <= 30; ++i) {
            ids.append(postId(i));
            posts.insert(postId(i), rootPost(i, server.ordinaryCount));
            context.push_back(postId(i));
        }
        channel.mergePostContext(ids, posts);
        ChannelPostSource raw(backend, channel);
        QSignalSpy requestsFinished(
            &raw, &AbstractPostSource::rangeRequestFinished);

        raw.requestRange(35, 39, AbstractPostSource::RequestReason::Initial, 0);
        QTRY_COMPARE_WITH_TIMEOUT(requestsFinished.size(), 1, 5000);
        QCOMPARE(raw.itemCount(), 40);
        QCOMPARE(raw.indexOfPost(postId(79)), 39);
        QVERIFY(raw.isPostPositionAuthoritative(postId(79)));
        const int tailIndex = raw.indexOfPost(postId(79));

        QSignalSpy inserted(&raw, &AbstractPostSource::itemsInserted);
        QVERIFY(raw.adoptNavigationContext(postId(15), context, false, false));

        // A single structural insertion creates the missing corridor without
        // overwriting the 10 verified newest roots.
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(inserted.at(0).at(0).toInt(), 30);
        QCOMPARE(inserted.at(0).at(1).toInt(), 3);
        QCOMPARE(raw.itemCount(), 43);
        QCOMPARE(raw.indexOfPost(postId(79)), tailIndex + 3);
        QVERIFY(raw.isPostPositionAuthoritative(postId(79)));
        for (int i = 0; i <= 30; ++i) {
            const int index = raw.indexOfPost(postId(i));
            QVERIFY2(index > 0, qPrintable(postId(i)));
            QCOMPARE(raw.postIdAt(index), postId(i));
            QVERIFY(raw.isAvailable(index));
            QVERIFY(!raw.isPostPositionAuthoritative(postId(i)));
        }

        FilteredPostSource visible(raw, mainTimelinePost);
        OutboxPostSource presentation(
            visible, PendingPostService::instance(backend),
            channel.id, QString());
        SourceList list(presentation);
        list.resize(500, 440);
        list.show();
        const int targetIndex = presentation.indexOfPost(postId(15));
        QVERIFY(targetIndex >= 0);
        list.scrollToIndex(targetIndex, LongListWidget::Alignment::Center);
        QTRY_VERIFY_WITH_TIMEOUT(
            list.widgetFor(presentation.indexOfPost(postId(15))), 5000);

        // After navigating an older unread context, going to the bottom must
        // keep walking past hidden system roots and replace the unresolved
        // provisional guard. A completed request with an unavailable final
        // row produced the blank tail in the reported trace.
        list.scrollToEnd();
        const auto tailMaterialized = [&] {
            const int last = presentation.itemCount() - 1;
            const auto visibleRange = list.visibleRange();
            if (last < 0 || !visibleRange.isValid()
                || visibleRange.last != last
                || presentation.postIdAt(last) != postId(21)) {
                return false;
            }
            for (int index = visibleRange.first; index <= visibleRange.last; ++index) {
                if (!presentation.isAvailable(index) || !list.widgetFor(index)) {
                    return false;
                }
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(tailMaterialized(), 10000);
    }

    void viewportLoadsPastHiddenPages_data()
    {
        QTest::addColumn<int>("ordinaryCount");
        QTest::addColumn<int>("hiddenCount");
        QTest::newRow("nineteen-counted-sixteen-hidden") << 19 << 16;
        QTest::newRow("initial-estimate-entirely-hidden") << 19 << 19;
        QTest::newRow("several-entirely-hidden-pages") << 3 << 40;
        QTest::newRow("large-channel") << 5000 << 16;
        QTest::newRow("large-hidden-tail") << 5000 << 40;
        QTest::newRow("only-hidden") << 0 << 16;
        QTest::newRow("empty") << 0 << 0;
        QTest::newRow("ordinary-small") << 3 << 0;
    }

    void viewportLoadsPastHiddenPages()
    {
        QFETCH(int, ordinaryCount);
        QFETCH(int, hiddenCount);
        HiddenHistoryServer server;
        server.ordinaryCount = ordinaryCount;
        server.hiddenCount = hiddenCount;
        QVERIFY(server.start());
        Backend backend;
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")}
        }, true);
        BackendChannel channel(backend.getStorage(), channelJson(ordinaryCount, hiddenCount));
        ChannelPostSource raw(backend, channel);
        FilteredPostSource visible(raw, mainTimelinePost);
        OutboxPostSource presentation(visible, PendingPostService::instance(backend),
                                     channel.id, QString());
        QSignalSpy remapped(&raw, &AbstractPostSource::layoutChanged);
        SourceList list(presentation);
        QSignalSpy demands(&list, &LongListWidget::rangeRequested);
        QSignalSpy completions(&presentation, &AbstractPostSource::rangeRequestFinished);
        list.resize(500, 440);
        list.show();
        list.scrollToEnd();
        const int newestOrdinary = ordinaryCount - 1;
        if (ordinaryCount > 0) {
            QTRY_VERIFY_WITH_TIMEOUT(visible.indexOfPost(postId(newestOrdinary)) >= 0, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(list.widgetFor(presentation.indexOfPost(postId(newestOrdinary))), 5000);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(visible.itemCount(), 0, 5000);
        }
        if (ordinaryCount > 0) {
            // Wait for the opening demand to finish filling the viewport before
            // expressing a new scroll intent. A prepend still in flight would
            // correctly preserve the old semantic anchor instead of following
            // the newly discovered oldest row.
            const auto viewportReady = [&] {
                const auto range = list.visibleRange();
                if (!range.isValid()) return false;
                for (int index = range.first; index <= range.last; ++index) {
                    if (!presentation.isAvailable(index) || !list.widgetFor(index)) return false;
                }
                return true;
            };
            QTRY_VERIFY_WITH_TIMEOUT(viewportReady(), 5000);
        }
        // Loading is driven by the viewport, not an automatic full-history
        // scan. Visit the top explicitly to verify the remaining small-channel
        // boundary, including a final full cursor page followed by one row.
        if (ordinaryCount > 0 && ordinaryCount < 100) {
            list.scrollToIndex(0, LongListWidget::Alignment::Top);
            QTRY_COMPARE_WITH_TIMEOUT(visible.postIdAt(0), postId(0), 5000);
            QCOMPARE(visible.itemCount(), ordinaryCount);
        }
        // A filled viewport may still have earlier prefetch/cache work in
        // flight. Wait for request completion and a quiet event-loop interval
        // before asserting the bounded final transport count. Sampling the
        // count immediately after materialization races platform scheduling.
        const auto settled = [&] {
            if (demands.size() != completions.size()) return false;
            const int requests = server.requests;
            QTest::qWait(100);
            return server.requests == requests && demands.size() == completions.size();
        };
        QTRY_VERIFY_WITH_TIMEOUT(settled(), 5000);
        QVERIFY(server.requests <= 9);
        QCOMPARE(remapped.size(), 0);
        // All materialized identities remain ordinary and correctly ordered.
        QString previous;
        for (int index = 0; index < visible.itemCount(); ++index) {
            const QString id = visible.postIdAt(index);
            if (id.isEmpty()) continue;
            QVERIFY(id <= postId(newestOrdinary));
            QVERIFY(previous.isEmpty() || previous < id);
            previous = id;
        }
        for (const QString& url : server.requestUrls) {
            const QUrlQuery query {QUrl(url)};
            // Ordinary newest opening and wheel demand stay on exact cursors.
            QCOMPARE(query.queryItemValue("page").toInt(), 0);
            QVERIFY(query.queryItemValue("per_page").toInt() <= 20);
        }
    }
};

QTEST_MAIN(ChannelPostSourceHiddenHistoryTest)
#include "ChannelPostSourceHiddenHistoryTest.moc"
