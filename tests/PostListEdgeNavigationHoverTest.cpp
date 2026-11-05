#include <QtTest>

#include <memory>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QFrame>
#include <QJsonObject>
#include <QScrollBar>
#include <QToolButton>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendPost.h"
#include "chat-area/PostListWidget.h"
#include "chat-area/post/PostWidget.h"
#include "ui/OverlayScrollBarManager.h"

using namespace Mattermost;

namespace {

void settleEvents(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

class TestPostList final : public PostListWidget
{
public:
    TestPostList(Backend& backend, std::vector<std::unique_ptr<BackendPost>>& posts)
        : backend_(backend), posts_(posts)
    {
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return new PostWidget(backend_, *posts_.at(index), viewport(), nullptr, nullptr);
    }

private:
    Backend& backend_;
    std::vector<std::unique_ptr<BackendPost>>& posts_;
};

std::vector<std::unique_ptr<BackendPost>> makePosts(Backend& backend)
{
    auto& storage = backend.getStorage();
    storage.addUser(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("test-user")},
        {QStringLiteral("username"), QStringLiteral("tester")},
        {QStringLiteral("first_name"), QStringLiteral("Test")}
    }, true);

    std::vector<std::unique_ptr<BackendPost>> posts;
    for (int i = 0; i < 20; ++i) {
        posts.push_back(std::make_unique<BackendPost>(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("post%1").arg(i, 22, 10, QLatin1Char('0'))},
            {QStringLiteral("channel_id"), QStringLiteral("test-channel")},
            {QStringLiteral("user_id"), QStringLiteral("test-user")},
            {QStringLiteral("message"), QStringLiteral("Hello, this is test post %1").arg(i)},
            {QStringLiteral("create_at"), double(100000 + i * 1000)}
        }, storage));
    }
    return posts;
}

QFrame* visibleActions(QWidget* viewport)
{
    for (QFrame* frame : viewport->findChildren<QFrame*>(
             QStringLiteral("postHoverActions"))) {
        if (frame->isVisible()) {
            return frame;
        }
    }
    return nullptr;
}

} // namespace

class PostListEdgeNavigationHoverTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        OverlayScrollBarManager::install(*qApp);
    }

    void quickScrollExclusionImmediatelyHidesAndRestoresHover()
    {
        Backend backend;
        auto posts = makePosts(backend);
        TestPostList list(backend, posts);
        list.resize(480, 230);
        list.setItemCount(static_cast<int>(posts.size()));
        list.setRangeAvailable(0, static_cast<int>(posts.size()) - 1);
        list.show();
        settleEvents();

        QVERIFY(list.verticalScrollBar()->maximum() > 0);
        auto* end = list.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        QVERIFY(end);
        QVERIFY(end->isVisible());

        const int middleIndex =
            list.indexAtViewportPosition(list.viewport()->height() / 2);
        QVERIFY(middleIndex >= 0);
        QTRY_VERIFY(list.itemWidget(middleIndex));
        auto* post = qobject_cast<PostWidget*>(list.itemWidget(middleIndex));
        QVERIFY(post);

        const QPoint rowPoint = post->mapToGlobal(post->rect().center());
        QCursor::setPos(rowPoint);
        QTest::mouseMove(post, post->rect().center());
        QEvent enter(QEvent::Enter);
        QCoreApplication::sendEvent(post, &enter);
        QTRY_VERIFY(visibleActions(list.viewport()));
        QFrame* actions = visibleActions(list.viewport());
        QVERIFY(actions);

        // The contour extends one button size to the left of the glyph.
        // The point is not a click target for the button, but it must stop
        // the row's hover toolbar from remaining above that click target.
        const QPoint edgePoint = end->mapToGlobal(end->rect().center())
            - QPoint(end->width(), 0);
        QVERIFY(OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
            list, edgePoint));
        QCursor::setPos(edgePoint);
        QTest::mouseMove(list.viewport(),
                         list.viewport()->mapFromGlobal(edgePoint));
        settleEvents();
        QVERIFY(!actions->isVisible());

        // A stale post Enter must not reopen the floating toolbar while the
        // cursor remains inside the invisible navigation exclusion contour.
        QCoreApplication::sendEvent(post, &enter);
        settleEvents();
        QVERIFY(!actions->isVisible());

        // A late Enter directed at the previously shown floating surface
        // must not reactivate the toolbar or its ranked emoji controls.
        QEvent staleToolbarEnter(QEvent::Enter);
        QCoreApplication::sendEvent(actions, &staleToolbarEnter);
        settleEvents();
        QVERIFY(!actions->isVisible());

        // Moving back into the SAME row must restore hover even without
        // another Qt row Enter event (the row itself never lost mouse focus).
        QCursor::setPos(rowPoint);
        QTest::mouseMove(post, post->rect().center());
        QTRY_VERIFY(actions->isVisible());

        // Crossing the contour again hides it without waiting for the
        // floating toolbar's normal leave-delay timer.
        const QPoint globalButton =
            end->mapToGlobal(end->rect().center());
        QCursor::setPos(globalButton);
        QTest::mouseMove(end, end->rect().center());
        settleEvents();
        QVERIFY(!actions->isVisible());

        // The physical icon must remain the click target even if the toolbar
        // was raised earlier and the mouse passes over its old location.
        QCursor::setPos(globalButton);
        settleEvents();
        QCOMPARE(QApplication::widgetAt(globalButton),
                 static_cast<QWidget*>(end));

        QTest::mouseClick(end, Qt::LeftButton);
        settleEvents();
        QCOMPARE(list.verticalScrollBar()->value(),
                 list.verticalScrollBar()->maximum());
        QVERIFY(!end->isVisible());
        QVERIFY(!OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
            list, globalButton));

        // A hidden navigation button must not block a post hover at the new
        // bottom of the log.
        const int lastIndex = static_cast<int>(posts.size()) - 1;
        QTRY_VERIFY(list.itemWidget(lastIndex));
        auto* lastPost = qobject_cast<PostWidget*>(list.itemWidget(lastIndex));
        QVERIFY(lastPost);
        const QPoint lastPoint = lastPost->mapToGlobal(lastPost->rect().center());
        QCursor::setPos(lastPoint);
        QTest::mouseMove(lastPost, lastPost->rect().center());
        QEvent enterLast(QEvent::Enter);
        QCoreApplication::sendEvent(lastPost, &enterLast);
        QTRY_VERIFY(visibleActions(list.viewport()));
    }
};

QTEST_MAIN(PostListEdgeNavigationHoverTest)
#include "PostListEdgeNavigationHoverTest.moc"
