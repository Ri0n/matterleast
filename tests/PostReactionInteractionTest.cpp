#include <algorithm>

#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QSignalSpy>
#include <QtTest>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/types/BackendPost.h"
#include "chat-area/post/PostWidget.h"
#include "chat-area/post/ReactionChipStyle.h"
#include "chat-area/post/reactions/PostReaction.h"
#include "chat-area/post/reactions/PostReactionList.h"

using namespace Mattermost;

class PostReactionInteractionTest : public QObject
{
    Q_OBJECT

private slots:
    void reactionCountTracksInheritedChatFont()
    {
        Backend backend;
        PostReactionList list(backend);
        list.addReaction(QStringLiteral("eyes"), QString::fromUtf8("👀"),
                         BackendPostReaction {QStringLiteral("Alice")});

        QFont chatFont = list.font();
        chatFont.setPointSize(19);
        list.setFont(chatFont);
        QApplication::processEvents();

        auto* chip = list.findChild<PostReaction*>();
        QVERIFY(chip);
        auto* count = chip->findChild<QLabel*>(QStringLiteral("count"));
        QVERIFY(count);
        const QFont expected = ReactionChipStyle::countFont(chatFont);
        QCOMPARE(count->font().pointSizeF(), expected.pointSizeF());

        auto* emoji = chip->findChild<QLabel*>(QStringLiteral("emoji"));
        QVERIFY(emoji);
        const int expectedExtent = ReactionChipStyle::iconExtent(chatFont);
        const int expectedBoxExtent = ReactionChipStyle::iconBoxExtent(chatFont);
        QCOMPARE(emoji->size(), QSize(expectedBoxExtent, expectedBoxExtent));
        QCOMPARE(emoji->font().pointSizeF(), chatFont.pointSizeF());
        QVERIFY(expectedBoxExtent > expectedExtent);
        QCOMPARE(chip->minimumHeight(), ReactionChipStyle::chipHeight(chatFont));
        QVERIFY(list.minimumHeight() >= ReactionChipStyle::chipHeight(chatFont));
        QVERIFY2(list.minimumHeight() > 27,
                 "Reaction list must not retain the legacy 27px height cap");
    }

    void reactionEventsAreIdempotentAndUseUserIds()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa");
        QJsonObject user {
            {QStringLiteral("id"), userId},
            {QStringLiteral("username"), QStringLiteral("alice")},
            {QStringLiteral("first_name"), QStringLiteral("Alice")},
        };
        BackendUser* loginUser = storage.addUser(user, true);
        QVERIFY(loginUser);
        loginUser->isLoginUser = true;

        QJsonArray reactionArray {
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("emoji_name"), QStringLiteral("eyes")},
            },
        };
        QJsonObject metadata {
            {QStringLiteral("reactions"), reactionArray},
        };
        QJsonObject postJson {
            {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
            {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
            {QStringLiteral("user_id"), userId},
            {QStringLiteral("create_at"), 1},
            {QStringLiteral("metadata"), metadata},
        };

        BackendPost post(postJson, storage);
        const EmojiID eyes = EmojiInfo::findByName(QStringLiteral("eyes"));
        QVERIFY(eyes);

        auto reaction = post.reactions.find(eyes);
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});

        // A replayed reaction_added event is a repeated fact, not a toggle.
        post.addReaction(userId, QStringLiteral("eyes"));
        reaction = post.reactions.find(eyes);
        QVERIFY(reaction != post.reactions.end());
        QCOMPARE(reaction->second, BackendPostReaction {userId});

        post.removeReaction(userId, QStringLiteral("eyes"));
        QVERIFY(post.reactions.find(eyes) == post.reactions.end());
    }

    void liveReactionUpdateCommitsPostGeometry()
    {
        Backend backend;
        Storage& storage = backend.getStorage();

        const QString userId = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaa");
        BackendUser* user = storage.addUser(
            QJsonObject {
                {QStringLiteral("id"), userId},
                {QStringLiteral("username"), QStringLiteral("alice")},
                {QStringLiteral("first_name"), QStringLiteral("Alice")},
            },
            true);
        QVERIFY(user);
        user->isLoginUser = true;

        BackendPost post(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbb")},
                {QStringLiteral("channel_id"), QStringLiteral("cccccccccccccccccccccccccc")},
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("message"), QStringLiteral("hello")},
            },
            storage);

        PostWidget widget(backend, post, nullptr, nullptr, nullptr);
        widget.resize(480, widget.sizeHint().height());
        widget.show();
        QApplication::processEvents();
        const int before = widget.sizeHint().height();

        QSignalSpy dimensions(&widget, &PostWidget::dimensionsChanged);
        post.addReaction(userId, QStringLiteral("eyes"));
        widget.updateReactions();

        QTRY_VERIFY_WITH_TIMEOUT(dimensions.count() > 0, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(widget.sizeHint().height() > before, 1000);

        auto* list = widget.findChild<PostReactionList*>();
        QVERIFY(list);
        QVERIFY(list->height() >= list->minimumSizeHint().height());
        QVERIFY(widget.rect().contains(
            list->mapTo(&widget, list->rect().bottomRight())));
    }

    void secondChipIsOneClickableHitTarget()
    {
        Backend backend;
        PostReactionList list(backend);
        list.addReaction(QStringLiteral("thumbsup"), QString::fromUtf8("👍"),
                         BackendPostReaction {QStringLiteral("Alice")});
        list.addReaction(QStringLiteral("eyes"), QString::fromUtf8("👀"),
                         BackendPostReaction {QStringLiteral("Bob")});
        list.resize(220, 32);
        list.show();
        QVERIFY(QTest::qWaitForWindowExposed(&list));
        QApplication::processEvents();

        auto chips = list.findChildren<PostReaction*>();
        QCOMPARE(chips.size(), 2);
        std::sort(chips.begin(), chips.end(), [](PostReaction* lhs, PostReaction* rhs) {
            return lhs->mapToGlobal(QPoint()).x() < rhs->mapToGlobal(QPoint()).x();
        });
        PostReaction* second = chips.at(1);
        const QPoint globalCenter = second->mapToGlobal(second->rect().center());
        QWidget* hit = QApplication::widgetAt(globalCenter);
        QCOMPARE(hit, static_cast<QWidget*>(second));

        QSignalSpy spy(&list, &PostReactionList::reactionClicked);
        QTest::mouseClick(hit, Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("eyes"));
    }
};

QTEST_MAIN(PostReactionInteractionTest)
#include "PostReactionInteractionTest.moc"
