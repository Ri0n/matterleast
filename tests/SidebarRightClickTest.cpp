#include <QtTest>

#include <QApplication>
#include <QContextMenuEvent>
#include <QSignalSpy>
#include <QTreeWidgetItem>

#include "channel-tree/AttentionList.h"
#include "channel-tree/ChannelQuickList.h"
#include "channel-tree/ChannelTree.h"
#include "channel-tree/SidebarItem.h"

using namespace Mattermost;

class SidebarRightClickTest : public QObject
{
    Q_OBJECT

private slots:
    void rightClickDoesNotSelectOrOpenAnotherConversation()
    {
        ChannelTree channels;
        ChannelQuickList unread;
        AttentionList attention;
        QTreeWidget* views[] = {&channels, &unread, &attention};

        for (QTreeWidget* view : views) {
            // Exercise mouse routing without opening a modal context menu.
            view->setContextMenuPolicy(Qt::NoContextMenu);
            view->setHeaderHidden(true);
            view->setRootIsDecorated(false);
            view->resize(360, 180);

            auto* active = new QTreeWidgetItem(view);
            active->setText(0, QStringLiteral("Current conversation"));
            active->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
            active->setData(0, SidebarItem::IdRole, QStringLiteral("current"));

            auto* other = new QTreeWidgetItem(view);
            other->setText(0, QStringLiteral("Other conversation"));
            other->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
            other->setData(0, SidebarItem::IdRole, QStringLiteral("other"));

            view->show();
            view->setCurrentItem(active);
            QSignalSpy selectionChanges(view, &QTreeWidget::currentItemChanged);
            const QRect otherRect = view->visualItemRect(other);
            QVERIFY(otherRect.isValid());
            QVERIFY(view->viewport()->rect().contains(otherRect.center()));

            QTest::mouseClick(view->viewport(), Qt::RightButton, Qt::NoModifier,
                              otherRect.center());
            QCOMPARE(view->currentItem(), active);
            QCOMPARE(selectionChanges.size(), 0);

            QTest::mouseClick(view->viewport(), Qt::RightButton, Qt::NoModifier,
                              view->visualItemRect(active).center());
            QCOMPARE(view->currentItem(), active);
            QCOMPARE(selectionChanges.size(), 0);

            // Ordinary left-click navigation is unaffected.
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                              otherRect.center());
            QCOMPARE(view->currentItem(), other);
            QCOMPARE(selectionChanges.size(), 1);
            view->hide();
        }
    }

    void contextMenuStillTargetsPointedFollowingRow()
    {
        ChannelQuickList view;
        view.resize(360, 160);
        auto* row = new QTreeWidgetItem(&view);
        row->setText(0, QStringLiteral("Target conversation"));
        row->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
        row->setData(0, SidebarItem::ChannelIdRole, QStringLiteral("target-id"));
        view.show();

        const QPoint pos = view.visualItemRect(row).center();
        QVERIFY(view.viewport()->rect().contains(pos));
        QSignalSpy requested(&view, &ChannelQuickList::channelContextMenuRequested);
        QContextMenuEvent context(QContextMenuEvent::Mouse, pos,
                                  view.viewport()->mapToGlobal(pos));
        QApplication::sendEvent(view.viewport(), &context);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.takeFirst().at(0).toString(), QStringLiteral("target-id"));
    }
};

QTEST_MAIN(SidebarRightClickTest)
#include "SidebarRightClickTest.moc"
