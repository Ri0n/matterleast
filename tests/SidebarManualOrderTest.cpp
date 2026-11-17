#include <QtTest>

#include "channel-tree/SidebarManualOrder.h"
#include "channel-tree/SidebarChannelMovePolicy.h"

using namespace Mattermost;

class SidebarManualOrderTest : public QObject
{
    Q_OBJECT

private slots:
    void alphaToManualPreservesDisplayedOrder()
    {
        const QStringList stored {QStringLiteral("z"), QStringLiteral("a"),
                                  QStringLiteral("c"), QStringLiteral("b")};
        const QStringList displayed {QStringLiteral("a"), QStringLiteral("b"),
                                     QStringLiteral("c"), QStringLiteral("z")};
        QCOMPARE(sidebarManualOrder(stored, displayed), displayed);
    }

    void dragInSortedCategoryUsesVisualAnchor()
    {
        QStringList order = sidebarManualOrder(
            {QStringLiteral("d"), QStringLiteral("c"),
             QStringLiteral("a"), QStringLiteral("b")},
            {QStringLiteral("a"), QStringLiteral("b"),
             QStringLiteral("c"), QStringLiteral("d")});
        QVERIFY(reorderSidebarChannel(order, QStringLiteral("d"),
                                      QStringLiteral("b"), true));
        QCOMPARE(order, (QStringList {QStringLiteral("a"), QStringLiteral("b"),
                                       QStringLiteral("d"), QStringLiteral("c")}));
    }

    void invisibleMembersArePreserved()
    {
        const QStringList order = sidebarManualOrder(
            {QStringLiteral("hidden1"), QStringLiteral("c"),
             QStringLiteral("a"), QStringLiteral("hidden2"),
             QStringLiteral("b")},
            {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        QCOMPARE(order, (QStringList {QStringLiteral("a"), QStringLiteral("b"),
                                      QStringLiteral("c"), QStringLiteral("hidden1"),
                                      QStringLiteral("hidden2")}));
    }

    void unknownAndDuplicateVisibleIdsCannotCorruptMembership()
    {
        QCOMPARE(sidebarManualOrder(
            {QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("c")},
            {QStringLiteral("a"), QStringLiteral("foreign"),
             QStringLiteral("a"), QStringLiteral("c")}),
            (QStringList {QStringLiteral("a"), QStringLiteral("c"),
                          QStringLiteral("b")}));
    }
};

QTEST_MAIN(SidebarManualOrderTest)
#include "SidebarManualOrderTest.moc"
