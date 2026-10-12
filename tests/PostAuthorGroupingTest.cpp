#include <QtTest>

#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QLocale>
#include <QPalette>
#include <QPushButton>
#include <QSizePolicy>

#include "Settings.h"
#include "options/MLOptions.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendPost.h"
#include "chat-area/post/PostAuthorRunPolicy.h"
#include "chat-area/post/PostTimestampPresentation.h"
#include "chat-area/post/PostWidget.h"

using namespace Mattermost;

namespace {

BackendPost makePost(Storage& storage,
                     const QString& id,
                     const QString& userId,
                     qint64 createAt,
                     const QString& rootId = {},
                     const QString& type = {})
{
    return BackendPost(
        QJsonObject {
            {QStringLiteral("id"), id},
            {QStringLiteral("channel_id"),
             QStringLiteral("cccccccccccccccccccccccccc")},
            {QStringLiteral("user_id"), userId},
            {QStringLiteral("create_at"), static_cast<double>(createAt)},
            {QStringLiteral("root_id"), rootId},
            {QStringLiteral("type"), type},
            {QStringLiteral("message"), QStringLiteral("message")},
        },
        storage);
}

qint64 localMs(const QDate& date, const QTime& time)
{
    return QDateTime(date, time).toMSecsSinceEpoch();
}

} // namespace

class PostAuthorGroupingTest : public QObject
{
    Q_OBJECT

private slots:
    void compatibleConsecutivePostsContinueRun()
    {
        Storage storage;
        const qint64 firstTime =
            localMs(QDate(2026, 9, 29), QTime(10, 0));
        auto first = makePost(
            storage, QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa"),
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu"), firstTime);
        auto second = makePost(
            storage, QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb"),
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu"),
            firstTime + 60 * 1000);

        QVERIFY(PostAuthorRunPolicy::continues(first, second));
    }

    void fiveMinuteWindowMatchesOfficialClient()
    {
        Storage storage;
        const qint64 base =
            localMs(QDate(2026, 9, 29), QTime(10, 0));
        const QString author =
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu");
        auto first = makePost(
            storage, QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa"),
            author, base);

        auto exactlyFiveMinutes = makePost(
            storage, QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb"),
            author, base + 5 * 60 * 1000);
        QVERIFY(PostAuthorRunPolicy::continues(first, exactlyFiveMinutes));

        auto beyondFiveMinutes = makePost(
            storage, QStringLiteral("cccccccccccccccccccccccccc"),
            author, base + 5 * 60 * 1000 + 1);
        QVERIFY(!PostAuthorRunPolicy::continues(first, beyondFiveMinutes));
    }

    void timestampPresentationMatchesMattermostContexts()
    {
        const QLocale locale(QStringLiteral("be_BY"));
        const qint64 now =
            localMs(QDate(2026, 9, 29), QTime(20, 0));

        QCOMPARE(
            PostTimestampPresentation::absoluteTime(now, locale),
            locale.toString(QTime(20, 0), QLocale::ShortFormat));

        QCOMPARE(
            PostTimestampPresentation::threadRelativeTime(
                now - 30 * 1000, now, locale),
            QStringLiteral("now"));
        QCOMPARE(
            PostTimestampPresentation::threadRelativeTime(
                now - 5 * 60 * 1000, now, locale),
            QStringLiteral("5 minutes ago"));
        QCOMPARE(
            PostTimestampPresentation::threadRelativeTime(
                now - 2 * 60 * 60 * 1000, now, locale),
            QStringLiteral("2 hours ago"));
        QCOMPARE(
            PostTimestampPresentation::threadRelativeTime(
                now - 2 * 24 * 60 * 60 * 1000, now, locale),
            QStringLiteral("2 days ago"));
    }

    void meaningfulBoundariesBreakRun()
    {
        Storage storage;
        const qint64 late =
            localMs(QDate(2026, 9, 29), QTime(23, 59));
        const QString author =
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu");

        auto base = makePost(
            storage, QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa"),
            author, late);

        auto otherAuthor = makePost(
            storage, QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb"),
            QStringLiteral("vvvvvvvvvvvvvvvvvvvvvvvvvv"), late + 1);
        QVERIFY(!PostAuthorRunPolicy::continues(base, otherAuthor));

        auto nextDay = makePost(
            storage, QStringLiteral("cccccccccccccccccccccccccc"),
            author,
            localMs(QDate(2026, 9, 30), QTime(0, 1)));
        QVERIFY(!PostAuthorRunPolicy::continues(base, nextDay));

        auto threadReply = makePost(
            storage, QStringLiteral("dddddddddddddddddddddddddd"),
            author, late + 1000,
            QStringLiteral("rrrrrrrrrrrrrrrrrrrrrrrrrr"));
        QVERIFY(!PostAuthorRunPolicy::continues(base, threadReply));

        auto systemPost = makePost(
            storage, QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeee"),
            author, late + 1000, {},
            QStringLiteral("system_join_channel"));
        QVERIFY(!PostAuthorRunPolicy::continues(base, systemPost));

        auto deleted = makePost(
            storage, QStringLiteral("ffffffffffffffffffffffffff"),
            author, late + 1000);
        deleted.isDeleted = true;
        QVERIFY(!PostAuthorRunPolicy::continues(base, deleted));
    }

    void authorUsesSameSizeAsConfiguredChatFont()
    {
        Backend backend;
        auto post = makePost(
            backend.getStorage(),
            QStringLiteral("pppppppppppppppppppppppppp"),
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu"),
            localMs(QDate(2026, 9, 29), QTime(12, 0)));
        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        auto* author = widget.findChild<QLabel*>(QStringLiteral("authorName"));
        QVERIFY(author);

        const QString configured = MLOptions::instance()->optionObject<QString>(
            CHAT_FONT, QApplication::font().toString())->value().toString();
        QFont expected;
        if (configured.isEmpty() || !expected.fromString(configured)) {
            expected = QApplication::font();
        }
        QCOMPARE(author->font().pointSizeF(), expected.pointSizeF());
        QVERIFY(author->font().bold());
        QVERIFY(author->maximumHeight() > author->fontMetrics().height());
    }

    void widgetCanMutateBetweenHeadAndContinuation()
    {
        Backend backend;
        auto post = makePost(
            backend.getStorage(),
            QStringLiteral("gggggggggggggggggggggggggg"),
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu"),
            localMs(QDate(2026, 9, 29), QTime(12, 0)));

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        auto* avatar = widget.findChild<QWidget*>(
            QStringLiteral("authorAvatar"));
        auto* authorName = widget.findChild<QLabel*>(
            QStringLiteral("authorName"));
        auto* time = widget.findChild<QLabel*>(QStringLiteral("time"));
        QVERIFY(avatar);
        QVERIFY(authorName);
        QVERIFY(time);

        const QDateTime postTime =
            QDateTime::fromMSecsSinceEpoch(post.create_at);
        const QLocale locale = QLocale::system();
        QCOMPARE(time->text(),
                 locale.toString(postTime.time(), QLocale::ShortFormat));
        QCOMPARE(time->toolTip(),
                 locale.toString(postTime.date(), QLocale::LongFormat)
                     + QStringLiteral(" ")
                     + locale.toString(postTime.time(), QLocale::LongFormat));

        QVERIFY(!widget.authorRunContinuation());
        QCOMPARE(avatar->minimumHeight(), 48);
        QCOMPARE(avatar->maximumHeight(), 48);
        QVERIFY(!authorName->isHidden());
        QVERIFY(!time->isHidden());
        QCOMPARE(time->sizePolicy().horizontalPolicy(), QSizePolicy::Maximum);
        QVERIFY(time->alignment() & Qt::AlignLeft);
        QVERIFY(time->font().pointSizeF() <= authorName->font().pointSizeF());
        QVERIFY(!time->font().bold());
        QVERIFY(time->palette().color(QPalette::WindowText).alphaF() > 0.70);
        QVERIFY(time->palette().color(QPalette::WindowText).alphaF() < 0.75);

        widget.setAuthorRunContinuation(true);
        QVERIFY(widget.authorRunContinuation());
        QCOMPARE(avatar->minimumWidth(), 48);
        QCOMPARE(avatar->maximumWidth(), 48);
        QCOMPARE(avatar->minimumHeight(), 0);
        QCOMPARE(avatar->maximumHeight(), 0);
        QVERIFY(authorName->isHidden());
        QVERIFY(time->isHidden());

        // Removal/filtering/loading can make the same surviving post a run head
        // again. The transition must restore presentation in-place.
        widget.setAuthorRunContinuation(false);
        QVERIFY(!widget.authorRunContinuation());
        QCOMPARE(avatar->minimumHeight(), 48);
        QCOMPARE(avatar->maximumHeight(), 48);
        QVERIFY(!authorName->isHidden());
        QVERIFY(!time->isHidden());

        auto* hoverActions =
            widget.findChild<QFrame*>(QStringLiteral("postHoverActions"));
        QVERIFY(hoverActions);
        auto* hoverOpacity =
            qobject_cast<QGraphicsOpacityEffect*>(hoverActions->graphicsEffect());
        QVERIFY(hoverOpacity);

        widget.setHovered(true, true);
        QVERIFY(!hoverActions->isHidden());
        QCOMPARE(hoverOpacity->opacity(), 1.0);

        widget.setHovered(false, true);
        QVERIFY(hoverActions->isHidden());
        QCOMPARE(hoverOpacity->opacity(), 0.0);
    }

    void expandedQuickBarKeepsReactionButtonStableThroughClick()
    {
        Backend backend;
        auto post = makePost(
            backend.getStorage(),
            QStringLiteral("iiiiiiiiiiiiiiiiiiiiiiiiii"),
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu"),
            localMs(QDate(2026, 9, 29), QTime(12, 15)));

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        widget.resize(640, 180);
        widget.show();
        widget.setHovered(true, true);
        QApplication::processEvents();

        auto* hoverActions =
            widget.findChild<QFrame*>(QStringLiteral("postHoverActions"));
        auto* quickSlot =
            widget.findChild<QWidget*>(QStringLiteral("reactionQuickBarSlot"));
        QVERIFY(hoverActions);
        QVERIFY(quickSlot);

        QPushButton* reactionButton = nullptr;
        for (QPushButton* button : hoverActions->findChildren<QPushButton*>()) {
            if (button->property("matterleastPostOwner").value<QObject*>() == &widget) {
                reactionButton = button;
                break;
            }
        }
        QVERIFY(reactionButton);

        QEvent enterReaction(QEvent::Enter);
        QCoreApplication::sendEvent(reactionButton, &enterReaction);
        QTRY_VERIFY(quickSlot->maximumWidth() > 0);
        const int expandedSlotWidth = quickSlot->maximumWidth();
        const int expandedToolbarWidth = hoverActions->width();

        // Avoid opening the real chooser in this regression test; we only need
        // QAbstractButton's click contract while the inline extension is open.
        QObject::disconnect(reactionButton, nullptr, &widget, nullptr);
        QSignalSpy clicked(reactionButton, &QPushButton::clicked);

        QTest::mousePress(reactionButton, Qt::LeftButton);
        // The old regression collapsed/moved the toolbar on press, before the
        // button could process its release, which could suppress clicked().
        QCOMPARE(quickSlot->maximumWidth(), expandedSlotWidth);

        QTest::mouseRelease(reactionButton, Qt::LeftButton);
        QCOMPARE(clicked.count(), 1);
        QTRY_COMPARE(quickSlot->maximumWidth(), 0);
        QTRY_VERIFY(hoverActions->width() < expandedToolbarWidth);
    }

    void widgetKeepsSnapshotLeaseUntilDeferredDelete()
    {
        Backend backend;
        auto snapshot = std::make_shared<BackendPost>(makePost(
            backend.getStorage(),
            QStringLiteral("hhhhhhhhhhhhhhhhhhhhhhhhhh"),
            QStringLiteral("uuuuuuuuuuuuuuuuuuuuuuuuuu"),
            localMs(QDate(2026, 9, 29), QTime(12, 30))));
        std::weak_ptr<BackendPost> weakSnapshot = snapshot;

        auto* widget = new PostWidget(
            backend, *snapshot, nullptr, nullptr, nullptr,
            PostWidget::PresentationMode::Interactive, snapshot);
        widget->deleteLater();
        snapshot.reset();

        // This is the collection reset window that previously produced a UAF:
        // the logical snapshot owner is gone, but Qt has not delivered the
        // PostWidget's DeferredDelete event yet.
        QVERIFY(!weakSnapshot.expired());

        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(weakSnapshot.expired());
    }
};

QTEST_MAIN(PostAuthorGroupingTest)
#include "PostAuthorGroupingTest.moc"
