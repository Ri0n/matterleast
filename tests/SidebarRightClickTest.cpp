#include <QtTest>

#include <QApplication>
#include <QContextMenuEvent>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTreeWidgetItem>

#include "backend/Backend.h"
#include "backend/FollowingModel.h"
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
        Backend first;
        Backend second;
        for (auto* owner : {&first, &second}) {
            owner->getStorage().addUser(
                QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
            QVERIFY(owner->getStorage().addGroupChannel(
                QJsonObject {{"id", "target-id"}, {"type", "G"},
                             {"display_name", "Target conversation"}}));
            FollowingModel::instance(*owner).markPostUnread(
                "target-id", "same-thread", "unread-post", 1000);
        }

        ChannelQuickList view;
        view.setUnreadFilters({true, false, false});
        view.setAccounts({{"a", &first}, {"b", &second}});
        view.resize(360, 160);
        view.show();
        QCOMPARE(view.topLevelItemCount(), 2);
        auto* active = view.topLevelItem(0);
        auto* target = view.topLevelItem(1);
        QVERIFY(active->data(0, SidebarItem::AccountIdRole)
                != target->data(0, SidebarItem::AccountIdRole));
        Backend* expected = target->data(0, SidebarItem::AccountIdRole).toString() == "a"
            ? &first : &second;
        view.setCurrentItem(active);

        int requests = 0;
        QString requestedChannel;
        Backend* requestedOwner = nullptr;
        QPoint requestedPosition;
        connect(&view, &ChannelQuickList::channelContextMenuRequested, &view,
                [&](const QString& channelId, const QPoint& globalPos, Backend* owner) {
            ++requests;
            requestedChannel = channelId;
            requestedOwner = owner;
            requestedPosition = globalPos;
        });
        const QPoint pos = view.visualItemRect(target).center();
        QVERIFY(view.viewport()->rect().contains(pos));
        const QPoint global = view.viewport()->mapToGlobal(pos);
        QContextMenuEvent context(QContextMenuEvent::Mouse, pos, global);
        QApplication::sendEvent(view.viewport(), &context);
        QCOMPARE(requests, 1);
        QCOMPARE(requestedChannel, QStringLiteral("target-id"));
        QCOMPARE(requestedOwner, expected);
        QCOMPARE(requestedPosition, global);
        QCOMPARE(view.currentItem(), active);
    }
};

QTEST_MAIN(SidebarRightClickTest)
#include "SidebarRightClickTest.moc"
