#include "ChannelItemDelegate.h"

#include <QApplication>
#include <QFontMetrics>
#include <QHash>
#include <QMetaObject>
#include <QMouseEvent>
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
// Keep names clear of the presence badge that protrudes past DM avatars.
constexpr int AvatarTextSpacing = 7;
constexpr int SidebarEdgeInset = 4;
constexpr int CategoryActionExtent = 20;
constexpr int CategoryActionHitWidth = 30;

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

bool isDraftsDestination(const QModelIndex& index)
{
    return index.data(SidebarItem::DestinationRole).toInt()
        == SidebarItem::DraftsDestination;
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
    result.rect.adjust(SidebarEdgeInset, before, -SidebarEdgeInset, -after);
    return result;
}

QIcon symbolicDestinationIcon(const QString& path, const QColor& color)
{
    static QHash<QString, QHash<QRgb, QIcon>> cache;
    auto& colorCache = cache[path];
    const QRgb key = color.rgba();
    auto it = colorCache.constFind(key);
    if (it != colorCache.cend()) {
        return *it;
    }

    QIcon icon = IconUtils::tintedSymbolicIcon(path, color);
    colorCache.insert(key, icon);
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
        QStyleOptionViewItem resolved(option);
        initStyleOption(&resolved, index);
        hint.setHeight(qMax(ChannelRowHeight, QFontMetrics(resolved.font).height() + 8));
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
    QStyleOptionViewItem content = contentOption(option, index);
#ifdef Q_OS_WIN
    // Native Windows highlight brushes sometimes produce white selected text
    // over a light gray selection. Use contrast-safe pairs while keeping
    // separate light/dark desktop palettes.
    if (content.state.testFlag(QStyle::State_Selected)
        && isConversationRow(index)) {
        const bool dark = content.palette.color(QPalette::Window).lightness() < 128;
        const QColor selectedBackground = dark ? QColor(58, 79, 105)
                                               : QColor(211, 227, 245);
        const QColor selectedText = dark ? QColor(250, 250, 252)
                                         : QColor(24, 40, 62);
        content.palette.setColor(QPalette::Highlight, selectedBackground);
        content.palette.setColor(QPalette::HighlightedText, selectedText);
    }
#endif
    if (content.rect.height() <= 0) {
        return;
    }

    painter->save();
    painter->setOpacity(painter->opacity() * (1.0 - collapse));

    if (!isConversationRow(index)) {
        const bool showAdd = index.data(SidebarItem::CategoryActionRole).toBool();
        const bool showMenu = index.data(SidebarItem::CategoryMenuRole).toBool();
        // Keep category titles out of the accessory hit areas, even when
        // fonts are enlarged or category names are long.
        QStyleOptionViewItem labelOption(content);
        labelOption.rect.adjust(0, 0, -(showAdd ? CategoryActionHitWidth : 0)
                                        - (showMenu ? CategoryActionHitWidth : 0), 0);
        if (index.data(SidebarItem::MutedRole).toBool()) {
            QColor mutedText = labelOption.palette.color(QPalette::Text);
            mutedText.setAlphaF(mutedText.alphaF() * 0.65);
            labelOption.palette.setColor(QPalette::Text, mutedText);
        }
        QStyledItemDelegate::paint(painter, labelOption, index);
        const auto drawAction = [painter, &content](const QString& resource,
                                                    int slot,
                                                    bool hovered) {
            const QRect hitRect(content.rect.right()
                                    - (slot + 1) * CategoryActionHitWidth + 1,
                                content.rect.top(), CategoryActionHitWidth,
                                content.rect.height());
            const QRect iconRect(hitRect.center().x() - CategoryActionExtent / 2,
                                 hitRect.center().y() - CategoryActionExtent / 2,
                                 CategoryActionExtent, CategoryActionExtent);
            painter->save();
            painter->setOpacity(painter->opacity() * (hovered ? 1.0 : 0.48));
            symbolicDestinationIcon(resource, content.palette.color(QPalette::Text))
                .paint(painter, iconRect);
            painter->restore();
        };
        // Do not draw an always-visible hamburger on every category row.
        // The subtle vertical ellipsis appears only while the pointer is over
        // this category, and remains in the existing, palette-aware hit slot.
        if (showMenu && index.data(SidebarItem::CategoryMenuHoveredRole).toBool()) {
            drawAction(QStringLiteral(":/icons/dots-vertical"), showAdd ? 1 : 0,
                       false);
        }
        if (showAdd) {
            drawAction(QStringLiteral(":/icons/add"), 0,
                       index.data(SidebarItem::CategoryActionHoveredRole).toBool());
        }
        painter->restore();
        return;
    }

    QStyleOptionViewItem base(content);
    initStyleOption(&base, index);
    const QString text = base.text;
    QIcon icon = base.icon;
    const int type = channelType(index);
    const bool selected = content.state.testFlag(QStyle::State_Selected);

    if (isSavedDestination(index) || isDraftsDestination(index)) {
        const QColor iconColor = selected
            ? content.palette.color(QPalette::HighlightedText)
            : content.palette.color(QPalette::Text);
        const QString iconPath = isSavedDestination(index)
            ? QStringLiteral(":/icons/bookmark")
            : QStringLiteral(":/icons/edit");
        icon = symbolicDestinationIcon(iconPath, iconColor);
    }

    if (type == BackendChannel::groupChannel) {
        const QString channelId = index.data(SidebarItem::IdRole).toString();
        const QString accountId = index.data(SidebarItem::AccountIdRole).toString();
        const auto key = qMakePair(accountId, channelId);
        if (!channelId.isEmpty() && !requestedGroupChannels.contains(key)) {
            requestedGroupChannels.insert(key);
            if (QObject* owner = parent()) {
                // A shared sidebar must not resolve a group's title from a
                // different server with an identical remote channel ID.
                if (owner->metaObject()->indexOfMethod(
                        "ensureGroupChannelDisplayName(QString,QString)") >= 0) {
                    QMetaObject::invokeMethod(owner,
                                              "ensureGroupChannelDisplayName",
                                              Qt::QueuedConnection,
                                              Q_ARG(QString, accountId),
                                              Q_ARG(QString, channelId));
                } else if (owner->metaObject()->indexOfMethod(
                               "ensureGroupChannelDisplayName(QString)") >= 0) {
                    QMetaObject::invokeMethod(owner,
                                              "ensureGroupChannelDisplayName",
                                              Qt::QueuedConnection,
                                              Q_ARG(QString, channelId));
                }
            }
        }
    }

    if (icon.isNull()) {
        icon = ChannelIcons::conversation(type);
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

        textLeft = iconRect.right() + 1 + AvatarTextSpacing;
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
    const QColor normalText = content.palette.color(QPalette::Text);
    const QColor mutedText = content.palette.color(QPalette::Disabled, QPalette::Text);
    QColor textColor = selected ? content.palette.color(QPalette::HighlightedText)
                                : (muted ? mutedText : normalText);
    if (!selected && !muted
        && index.data(SidebarItem::KindRole).toInt() == SidebarItem::Channel
        && !index.data(SidebarItem::UnreadRole).toBool()
        && !index.data(SidebarItem::MentionedRole).toBool()) {
        // The palette already defines the muted endpoint for this theme.
        // Leave selected, unread and mentioned conversation names at full contrast.
        textColor = QColor::fromRgbF(
            (normalText.redF() + mutedText.redF()) / 2.0,
            (normalText.greenF() + mutedText.greenF()) / 2.0,
            (normalText.blueF() + mutedText.blueF()) / 2.0,
            (normalText.alphaF() + mutedText.alphaF()) / 2.0);
    }
    painter->setPen(textColor);

    const QFontMetrics metrics(font);
    const QPixmap statusEmoji = index.data(
        SidebarItem::CustomStatusEmojiRole).value<QPixmap>();
    const int emojiSize = qMin(metrics.height(), 18);
    const int gap = metrics.horizontalAdvance(QLatin1Char(' '));
    const bool showEmoji = !statusEmoji.isNull()
        && textRect.width() >= emojiSize + gap + metrics.averageCharWidth() * 4;
    const int nameWidth = showEmoji
        ? qMax(0, textRect.width() - emojiSize - gap) : textRect.width();
    const QString elided = metrics.elidedText(text, Qt::ElideRight, nameWidth);
    painter->drawText(QRect(textRect.left(), textRect.top(), nameWidth,
                            textRect.height()),
                      Qt::AlignLeft | Qt::AlignVCenter, elided);
    if (showEmoji) {
        const int namePixels = qMin(metrics.horizontalAdvance(elided), nameWidth);
        const QRect emojiRect(textRect.left() + namePixels + gap,
                              textRect.center().y() - emojiSize / 2,
                              emojiSize, emojiSize);
        painter->drawPixmap(emojiRect, statusEmoji);
    }
    painter->restore();
}

bool ChannelItemDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                      const QStyleOptionViewItem& option,
                                      const QModelIndex& index)
{
    if (event && event->type() == QEvent::MouseButtonRelease
        && index.data(SidebarItem::CategoryMenuRole).toBool()) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const bool showAdd =
                index.data(SidebarItem::CategoryActionRole).toBool();
            const auto slotRect = [&option](int slot) {
                return QRect(option.rect.right()
                                 - (slot + 1) * CategoryActionHitWidth + 1,
                             option.rect.top(), CategoryActionHitWidth,
                             option.rect.height());
            };
            if (showAdd && slotRect(0).contains(mouse->pos())) {
                emit categoryActionRequested(
                    index.data(SidebarItem::AccountIdRole).toString(),
                    index.data(SidebarItem::TeamIdRole).toString(),
                    index.data(SidebarItem::IdRole).toString());
                return true;
            }
            if (slotRect(showAdd ? 1 : 0).contains(mouse->pos())) {
                emit categoryMenuRequested(
                    index.data(SidebarItem::AccountIdRole).toString(),
                    index.data(SidebarItem::TeamIdRole).toString(),
                    index.data(SidebarItem::IdRole).toString());
                return true;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

} // namespace Mattermost
