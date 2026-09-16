#include "ChannelItemDelegate.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHash>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QtMath>

#include "ChannelIcons.h"
#include "SidebarItem.h"
#include "backend/types/BackendChannel.h"
#include "ui/AvatarUtils.h"
#include "ui/IconUtils.h"

namespace Mattermost {

namespace {

constexpr int ChannelRowHeight = 32;
constexpr int AvatarSize = 24;
constexpr int StatusSize = 12;
constexpr int MuteIconSize = 16;
constexpr int HorizontalMargin = 4;
constexpr int ItemSpacing = 4;

bool isTeamRow(const QModelIndex& index)
{
    return index.data(SidebarItem::KindRole).toInt() == SidebarItem::Team;
}

int channelType(const QModelIndex& index)
{
    return index.data(SidebarItem::ChannelTypeRole).toInt();
}

bool isConversationRow(const QModelIndex& index)
{
    const int kind = index.data(SidebarItem::KindRole).toInt();
    return kind == SidebarItem::Channel
        || kind == SidebarItem::Thread
        || kind == SidebarItem::VirtualDestination;
}

bool isSavedDestination(const QModelIndex& index)
{
    return index.data(SidebarItem::DestinationRole).toInt()
        == SidebarItem::SavedDestination;
}

int transientGap(const QModelIndex& index, int role)
{
    return qMax(0, index.data(role).toInt());
}

qreal collapseProgress(const QModelIndex& index)
{
    return qBound<qreal>(0.0, index.data(SidebarItem::DragCollapseRole).toDouble(), 1.0);
}

QStyleOptionViewItem contentOption(const QStyleOptionViewItem& option,
                                   const QModelIndex& index)
{
    QStyleOptionViewItem result(option);
    const int before = transientGap(index, SidebarItem::DropGapBeforeRole);
    const int after = transientGap(index, SidebarItem::DropGapAfterRole);
    result.rect.adjust(0, before, 0, -after);
    return result;
}

QIcon savedDestinationIcon(const QColor& color)
{
    static QHash<QRgb, QIcon> cache;
    const QRgb key = color.rgba();
    auto it = cache.constFind(key);
    if (it != cache.cend()) {
        return *it;
    }

    QIcon icon = IconUtils::tintedSymbolicIcon(
        QStringLiteral(":/icons/bookmark"), color);
    cache.insert(key, icon);
    return icon;
}

} // namespace

ChannelItemDelegate::ChannelItemDelegate(QObject* parent)
    : QStyledItemDelegate(parent)
{
}

void ChannelItemDelegate::initStyleOption(QStyleOptionViewItem* option,
                                          const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    if (!isConversationRow(index)) {
        return;
    }

    const bool unread = index.data(SidebarItem::UnreadRole).toBool();
    const bool mentioned = index.data(SidebarItem::MentionedRole).toBool();
    option->font.setBold(unread || mentioned);
}

QSize ChannelItemDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
{
    QSize hint = QStyledItemDelegate::sizeHint(option, index);
    if (isTeamRow(index)) {
        hint.setHeight(0);
        return hint;
    }
    if (isConversationRow(index)) {
        hint.setHeight(ChannelRowHeight);
    }

    const int before = transientGap(index, SidebarItem::DropGapBeforeRole);
    const int after = transientGap(index, SidebarItem::DropGapAfterRole);
    const qreal collapse = collapseProgress(index);
    const int contentHeight = qMax(0, qRound(hint.height() * (1.0 - collapse)));
    hint.setHeight(contentHeight + before + after);
    return hint;
}

void ChannelItemDelegate::paint(QPainter* painter,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    if (isTeamRow(index) || index.data(SidebarItem::DragSourceHiddenRole).toBool()) {
        return;
    }

    const qreal collapse = collapseProgress(index);
    const QStyleOptionViewItem content = contentOption(option, index);
    if (content.rect.height() <= 0) {
        return;
    }

    painter->save();
    painter->setOpacity(painter->opacity() * (1.0 - collapse));

    if (!isConversationRow(index)) {
        QStyledItemDelegate::paint(painter, content, index);
        painter->restore();
        return;
    }

    QStyleOptionViewItem base(content);
    initStyleOption(&base, index);
    const QString text = base.text;
    QIcon icon = base.icon;
    const int type = channelType(index);
    const bool selected = content.state.testFlag(QStyle::State_Selected);

    if (isSavedDestination(index)) {
        const QColor iconColor = selected
            ? content.palette.color(QPalette::HighlightedText)
            : content.palette.color(QPalette::Text);
        icon = savedDestinationIcon(iconColor);
    }

    if (type == BackendChannel::groupChannel) {
        const QString channelId = index.data(SidebarItem::IdRole).toString();
        if (!channelId.isEmpty() && !requestedGroupChannels.contains(channelId)) {
            requestedGroupChannels.insert(channelId);
            if (QObject* owner = parent()) {
                QMetaObject::invokeMethod(owner,
                                          "ensureGroupChannelDisplayName",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, channelId));
            }
        }
    }

    if (icon.isNull()) {
        if (type == BackendChannel::groupChannel) {
            icon = ChannelIcons::groupConversation();
        } else if (type == BackendChannel::publicChannel
                   || type == BackendChannel::privateChannel) {
            icon = ChannelIcons::channel();
        }
    }

    base.text.clear();
    base.icon = QIcon();

    const QStyle* style = content.widget ? content.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &base, painter, content.widget);

    QRect contentRect = content.rect.adjusted(HorizontalMargin, 0, -HorizontalMargin, 0);
    int textLeft = contentRect.left();

    if (!icon.isNull()) {
        const QRect iconRect(textLeft,
                             contentRect.center().y() - AvatarSize / 2,
                             AvatarSize,
                             AvatarSize);
        const QPixmap pixmap = icon.pixmap(AvatarSize, AvatarSize);

        const QString status = type == BackendChannel::directChannel
            ? index.data(SidebarItem::PresenceRole).toString()
            : QString();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (!status.isEmpty()) {
            QPainterPath clip;
            clip.addEllipse(iconRect);
            painter->setClipPath(clip);
        }
        painter->drawPixmap(iconRect, pixmap);
        painter->restore();

        if (!status.isEmpty()) {
            const QRect statusRect(iconRect.right() - StatusSize + 3,
                                   iconRect.bottom() - StatusSize + 3,
                                   StatusSize,
                                   StatusSize);
            const QColor badgeBackground = selected
                ? content.palette.color(QPalette::Highlight)
                : content.palette.color(QPalette::Base);
            AvatarUtils::drawStatusBadge(painter, statusRect, status, badgeBackground);
        }

        textLeft = iconRect.right() + 1 + ItemSpacing;
    }

    int textRight = contentRect.right();
    if (index.data(SidebarItem::MutedRole).toBool()) {
        const QRect muteRect(textRight - MuteIconSize + 1,
                             contentRect.center().y() - MuteIconSize / 2,
                             MuteIconSize,
                             MuteIconSize);
        style->standardIcon(QStyle::SP_MediaVolumeMuted).paint(painter, muteRect);
        textRight = muteRect.left() - ItemSpacing;
    }

    QRect textRect(textLeft, contentRect.top(),
                   qMax(0, textRight - textLeft + 1), contentRect.height());
    const QFont font = base.font;
    painter->setFont(font);

    const bool muted = index.data(SidebarItem::MutedRole).toBool();
    const QColor textColor = selected
        ? content.palette.color(QPalette::HighlightedText)
        : (muted ? content.palette.color(QPalette::Disabled, QPalette::Text)
                 : content.palette.color(QPalette::Text));
    painter->setPen(textColor);

    const QString elided = QFontMetrics(font).elidedText(text, Qt::ElideRight, textRect.width());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
    painter->restore();
}

} // namespace Mattermost
