/**
 * @file ChannelItem.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Mar 30, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MatterLeast is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MatterLeast. if not, see https://www.gnu.org/licenses/.
 */

#include "ChannelItem.h"

#include <QMenu>
#include <QTreeWidget>

#include "ChannelIcons.h"
#include "ChannelItemWidget.h"
#include "SidebarItem.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "ui/ConversationActionsMenu.h"

namespace Mattermost {

ChannelItem::ChannelItem (Backend& backend, ChannelItemWidget* itemWidget)
:ChannelTreeItem ()
,backend (backend)
,widget (itemWidget)
{
	QFont font1;
	font1.setBold (true);
	setFont (1, font1);
}

ChannelItem::~ChannelItem () = default;

void ChannelItem::setIcon (const QIcon& icon)
{
    // Private channels are semantic rows, not arbitrary icon holders. Some
    // sidebar construction paths still assign the generic channel icon after
    // setLabel(); normalize here so a private row cannot be downgraded back to
    // the public-channel icon. Direct/group conversations are unaffected.
    QIcon effectiveIcon = icon;
    if (BackendChannel* channel = backendChannel();
        channel && channel->type == BackendChannel::privateChannel) {
        effectiveIcon = ChannelIcons::conversation(channel->type);
    }

    QTreeWidgetItem::setIcon(0, effectiveIcon);
	if (widget) {
		widget->setIcon (effectiveIcon);
	}
}

void ChannelItem::setLabel (const QString& label)
{
    setText(0, label);
	if (widget) {
		widget->setLabel (label);
	}

    // ItemIdRole is assigned before setLabel() when the ChannelTree row is
    // materialized. Persist the channel type in model data as well so delegates
    // and alternate views never have to recover row semantics from their parent
    // widget or from the icon/presence fields.
    BackendChannel* channel = backendChannel();
    if (channel) {
        setData(0, SidebarItem::ChannelTypeRole, channel->type);
    }

    // Direct conversations get a real user avatar as soon as the profile is
    // resolved. Give every other row a stable semantic icon immediately so
    // public/private channels and group DMs do not look visually unfinished.
    if (QTreeWidgetItem::icon(0).isNull() && channel
        && channel->type != BackendChannel::directChannel) {
        setIcon(ChannelIcons::conversation(channel->type));
    }
}

void ChannelItem::setWidget (ChannelItemWidget* itemWidget)
{
	widget = itemWidget;
}

void ChannelItem::setMuted(bool muted)
{
    setData(0, SidebarItem::MutedRole, muted);
    if (widget) {
        widget->setMuted(muted);
    }
}

void ChannelItem::setUnread(bool unread)
{
    setData(0, SidebarItem::UnreadRole, unread);
}

void ChannelItem::setMentioned(bool mentioned)
{
    setData(0, SidebarItem::MentionedRole, mentioned);
    if (widget) {
        widget->setMentioned(mentioned);
    }
}

void ChannelItem::setStatus(const QString& status)
{
    setData(0, SidebarItem::PresenceRole, status);
}

BackendChannel* ChannelItem::backendChannel() const
{
    const QString channelId = data(0, SidebarItem::IdRole).toString();
    return channelId.isEmpty() ? nullptr : backend.getStorage().getChannelById(channelId);
}

void ChannelItem::showContextMenu(const QPoint& globalPos)
{
    const BackendChannel* channel = backendChannel();
    if (!channel) {
        return;
    }
    QMenu menu(treeWidget());
    menu.setObjectName(QStringLiteral("conversationActionsMenu"));
    ConversationActionsMenu::populate(
        menu, backend, channel->id,
        data(0, SidebarItem::TeamIdRole).toString(), treeWidget());
    menu.exec(globalPos);
}

} /* namespace Mattermost */
