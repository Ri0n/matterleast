#include <QtTest>

#include "backend/DirectConversationSidebarPolicy.h"
#include "channel-tree/SidebarChannelMovePolicy.h"

using namespace Mattermost;

class SidebarChannelMovePolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void reordersBySemanticChannelIdentity()
    {
        QStringList ids {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        QVERIFY(reorderSidebarChannel(ids, QStringLiteral("c"), QStringLiteral("a"), false));
        QCOMPARE(ids, QStringList({QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b")}));
    }

    void reordersAfterTarget()
    {
        QStringList ids {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        QVERIFY(reorderSidebarChannel(ids, QStringLiteral("a"), QStringLiteral("b"), true));
        QCOMPARE(ids, QStringList({QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("c")}));
    }

    void adjacentChannelBoundariesAreNoOps()
    {
        const QStringList original {QStringLiteral("a"), QStringLiteral("b"),
                                    QStringLiteral("c")};

        QStringList afterPrevious = original;
        QVERIFY(!reorderSidebarChannel(afterPrevious, QStringLiteral("b"),
                                       QStringLiteral("a"), true));
        QCOMPARE(afterPrevious, original);

        QStringList beforeNext = original;
        QVERIFY(!reorderSidebarChannel(beforeNext, QStringLiteral("b"),
                                       QStringLiteral("c"), false));
        QCOMPARE(beforeNext, original);
    }

    void reordersCategoriesBeforeTarget()
    {
        QStringList ids {QStringLiteral("favorites"), QStringLiteral("channels"),
                         QStringLiteral("direct")};
        QVERIFY(reorderSidebarCategory(ids, QStringLiteral("direct"),
                                       QStringLiteral("channels"), false));
        QCOMPARE(ids, QStringList({QStringLiteral("favorites"), QStringLiteral("direct"),
                                  QStringLiteral("channels")}));
    }

    void reordersCategoriesAfterTarget()
    {
        QStringList ids {QStringLiteral("favorites"), QStringLiteral("channels"),
                         QStringLiteral("direct")};
        QVERIFY(reorderSidebarCategory(ids, QStringLiteral("favorites"),
                                       QStringLiteral("channels"), true));
        QCOMPARE(ids, QStringList({QStringLiteral("channels"), QStringLiteral("favorites"),
                                  QStringLiteral("direct")}));
    }

    void adjacentCategoryBoundariesAreNoOps()
    {
        const QStringList original {QStringLiteral("favorites"), QStringLiteral("channels"),
                                    QStringLiteral("direct")};

        QStringList afterPrevious = original;
        QVERIFY(!reorderSidebarCategory(afterPrevious, QStringLiteral("channels"),
                                        QStringLiteral("favorites"), true));
        QCOMPARE(afterPrevious, original);

        QStringList beforeNext = original;
        QVERIFY(!reorderSidebarCategory(beforeNext, QStringLiteral("channels"),
                                        QStringLiteral("direct"), false));
        QCOMPARE(beforeNext, original);
    }

    void movesBetweenCategoriesWithoutDuplicates()
    {
        QStringList source {QStringLiteral("a"), QStringLiteral("b")};
        QStringList target {QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("d")};
        QVERIFY(moveSidebarChannel(source, target, QStringLiteral("a"), QStringLiteral("d"), false));
        QCOMPARE(source, QStringList({QStringLiteral("b")}));
        QCOMPARE(target, QStringList({QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("d")}));
    }

    void categoryHeaderDropAppends()
    {
        QStringList source {QStringLiteral("a"), QStringLiteral("b")};
        QStringList target {QStringLiteral("c"), QStringLiteral("d")};
        QVERIFY(moveSidebarChannel(source, target, QStringLiteral("a")));
        QCOMPARE(source, QStringList({QStringLiteral("b")}));
        QCOMPARE(target, QStringList({QStringLiteral("c"), QStringLiteral("d"), QStringLiteral("a")}));
    }

    void extractsDirectPeerPreferenceKey()
    {
        QCOMPARE(directConversationPeerUserId(QStringLiteral("me"),
                                              QStringLiteral("me__peer")),
                 QStringLiteral("peer"));
        QCOMPARE(directConversationPeerUserId(QStringLiteral("me"),
                                              QStringLiteral("peer__me")),
                 QStringLiteral("peer"));
        QCOMPARE(directConversationPeerUserId(QStringLiteral("me"),
                                              QStringLiteral("me__me")),
                 QStringLiteral("me"));
        QVERIFY(directConversationPeerUserId(QStringLiteral("me"),
                                             QStringLiteral("other__peer")).isEmpty());
        QVERIFY(directConversationPeerUserId(QStringLiteral("me"),
                                             QStringLiteral("malformed")).isEmpty());
    }

    void promotesRealtimeConversationWithoutDuplicates()
    {
        QStringList ids {QStringLiteral("old"), QStringLiteral("active"),
                         QStringLiteral("older"), QStringLiteral("active")};
        QVERIFY(promoteSidebarConversation(ids, QStringLiteral("active")));
        QCOMPARE(ids, QStringList({QStringLiteral("active"), QStringLiteral("old"),
                                  QStringLiteral("older")}));
        QVERIFY(!promoteSidebarConversation(ids, QStringLiteral("active")));

        QVERIFY(promoteSidebarConversation(ids, QStringLiteral("new")));
        QCOMPARE(ids, QStringList({QStringLiteral("new"), QStringLiteral("active"),
                                  QStringLiteral("old"), QStringLiteral("older")}));
        QVERIFY(!promoteSidebarConversation(ids, QString()));
    }
};

QTEST_APPLESS_MAIN(SidebarChannelMovePolicyTest)
#include "SidebarChannelMovePolicyTest.moc"
