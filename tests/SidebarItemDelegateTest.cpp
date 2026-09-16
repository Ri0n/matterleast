#include <QtTest>

#include <QImage>
#include <QListView>
#include <QPainter>
#include <QStandardItemModel>

#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/SidebarItem.h"

using namespace Mattermost;

class TestableChannelItemDelegate : public ChannelItemDelegate
{
public:
    using ChannelItemDelegate::ChannelItemDelegate;
    using ChannelItemDelegate::initStyleOption;
};

class SidebarItemDelegateTest : public QObject
{
    Q_OBJECT

private:
    static QImage renderItem(SidebarItem::Kind kind, int channelType, const QString& presence,
                             bool unread = false)
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(kind, SidebarItem::KindRole);
        item->setData(channelType, SidebarItem::ChannelTypeRole);
        item->setData(presence, SidebarItem::PresenceRole);
        item->setData(unread, SidebarItem::UnreadRole);

        QPixmap avatar(24, 24);
        avatar.fill(Qt::black);
        item->setIcon(QIcon(avatar));
        model.appendRow(item);

        // A styled item delegate is normally invoked by an item view. Keep the
        // unit test on that real contract as well: some platform styles expect
        // option.widget to be a valid view while painting CE_ItemViewItem.
        QListView view;
        view.setModel(&model);

        QStyleOptionViewItem option;
        option.initFrom(&view);
        option.widget = &view;
        option.rect = QRect(0, 0, 240, 32);
        option.state = QStyle::State_Enabled;

        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        ChannelItemDelegate delegate;
        delegate.paint(&painter, option, model.index(0, 0));
        painter.end();
        return image;
    }

private slots:
    void ignoresPresenceForNonUserRows()
    {
        const auto publicWithPresence = renderItem(SidebarItem::Channel,
                                                   BackendChannel::publicChannel,
                                                   QStringLiteral("online"));
        const auto publicWithoutPresence = renderItem(SidebarItem::Channel,
                                                      BackendChannel::publicChannel,
                                                      QString());
        QVERIFY(publicWithPresence == publicWithoutPresence);

        const auto privateWithPresence = renderItem(SidebarItem::Channel,
                                                    BackendChannel::privateChannel,
                                                    QStringLiteral("away"));
        const auto privateWithoutPresence = renderItem(SidebarItem::Channel,
                                                       BackendChannel::privateChannel,
                                                       QString());
        QVERIFY(privateWithPresence == privateWithoutPresence);

        const auto groupWithPresence = renderItem(SidebarItem::Channel,
                                                  BackendChannel::groupChannel,
                                                  QStringLiteral("dnd"));
        const auto groupWithoutPresence = renderItem(SidebarItem::Channel,
                                                     BackendChannel::groupChannel,
                                                     QString());
        QVERIFY(groupWithPresence == groupWithoutPresence);
    }

    void unreadRoleMakesConversationVisuallyBold()
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(SidebarItem::Channel, SidebarItem::KindRole);
        item->setData(BackendChannel::publicChannel, SidebarItem::ChannelTypeRole);
        model.appendRow(item);

        QListView view;
        view.setModel(&model);
        TestableChannelItemDelegate delegate;

        QStyleOptionViewItem readOption;
        readOption.initFrom(&view);
        delegate.initStyleOption(&readOption, model.index(0, 0));
        QVERIFY(!readOption.font.bold());

        item->setData(true, SidebarItem::UnreadRole);
        QStyleOptionViewItem unreadOption;
        unreadOption.initFrom(&view);
        delegate.initStyleOption(&unreadOption, model.index(0, 0));
        QVERIFY(unreadOption.font.bold());

        item->setData(false, SidebarItem::UnreadRole);
        item->setData(true, SidebarItem::MentionedRole);
        QStyleOptionViewItem mentionedOption;
        mentionedOption.initFrom(&view);
        delegate.initStyleOption(&mentionedOption, model.index(0, 0));
        QVERIFY(mentionedOption.font.bold());
    }

    void dragGapParticipatesInRowGeometry()
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(SidebarItem::Channel, SidebarItem::KindRole);
        model.appendRow(item);

        QListView view;
        view.setModel(&model);
        ChannelItemDelegate delegate;
        QStyleOptionViewItem option;
        option.initFrom(&view);

        const QModelIndex index = model.index(0, 0);
        QCOMPARE(delegate.sizeHint(option, index).height(), 32);

        item->setData(12, SidebarItem::DropGapBeforeRole);
        item->setData(8, SidebarItem::DropGapAfterRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), 52);

        item->setData(0.5, SidebarItem::DragCollapseRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), 36);
    }

    void sourcePlaceholderPreservesOriginalRowExtent()
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(SidebarItem::Channel, SidebarItem::KindRole);
        model.appendRow(item);

        QListView view;
        view.setModel(&model);
        ChannelItemDelegate delegate;
        QStyleOptionViewItem option;
        option.initFrom(&view);

        const QModelIndex index = model.index(0, 0);
        const int originalHeight = delegate.sizeHint(option, index).height();
        QCOMPARE(originalHeight, 32);

        item->setData(1.0, SidebarItem::DragCollapseRole);
        item->setData(originalHeight, SidebarItem::DropGapBeforeRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), originalHeight);

        item->setData(0, SidebarItem::DropGapBeforeRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), 0);
    }

    void rendersPresenceForDirectMessage()
    {
        const auto withPresence = renderItem(SidebarItem::Channel,
                                             BackendChannel::directChannel,
                                             QStringLiteral("online"));
        const auto withoutPresence = renderItem(SidebarItem::Channel,
                                                BackendChannel::directChannel,
                                                QString());
        QVERIFY(withPresence != withoutPresence);
    }

    void rendersVirtualDestinationAsConversationRow()
    {
        const auto channel = renderItem(SidebarItem::Channel,
                                        BackendChannel::directChannel,
                                        QString());
        const auto destination = renderItem(SidebarItem::VirtualDestination,
                                            BackendChannel::directChannel,
                                            QString());
        QCOMPARE(destination, channel);
    }

    void rendersVirtualDirectDestinationWithSamePresenceChrome()
    {
        const auto channel = renderItem(SidebarItem::Channel,
                                        BackendChannel::directChannel,
                                        QStringLiteral("online"));
        const auto destination = renderItem(SidebarItem::VirtualDestination,
                                            BackendChannel::directChannel,
                                            QStringLiteral("online"));
        QCOMPARE(destination, channel);
    }
};

QTEST_MAIN(SidebarItemDelegateTest)

#include "SidebarItemDelegateTest.moc"
