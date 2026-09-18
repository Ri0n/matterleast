#include <algorithm>

#include <QApplication>
#include <QFontMetrics>
#include <QLabel>
#include <QSignalSpy>
#include <QtTest>

#include "backend/Backend.h"
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
        QCOMPARE(emoji->size(), QSize(expectedExtent, expectedExtent));
        QCOMPARE(chip->minimumHeight(), ReactionChipStyle::chipHeight(chatFont));
        QVERIFY(list.minimumHeight() >= ReactionChipStyle::chipHeight(chatFont));
        QVERIFY2(list.minimumHeight() > 27,
                 "Reaction list must not retain the legacy 27px height cap");
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
