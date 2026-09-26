/**
 * @file GroupTeamItem.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Jun 21, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "GroupTeamItem.h"

#include <QMenu>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "channel-tree/channel-item/GroupChannelItem.h"
#include "channel-tree-dialogs/TeamChannelsListDialog.h"
#include "channel-tree-dialogs/UserSearchDialog.h"
#include "channel-tree-dialogs/ViewTeamMembersListDialog.h"

namespace Mattermost {

ChannelItem* GroupTeamItem::createChannelItem (Backend& backendRef, ChannelItemWidget* itemWidget)
{
	return new GroupChannelItem (backendRef, itemWidget);
}

void GroupTeamItem::showContextMenu (const QPoint& pos)
{
	QMenu myMenu;

	myMenu.addAction ("View Team Members", [this] {
		BackendTeam* team = backend.getStorage().getTeamById(teamId);
		if (!team) {
			return;
		}

		ViewTeamMembersDialog* dialog = new ViewTeamMembersListDialog (backend, *team, treeWidget());
		dialog->show ();
	});

	myMenu.addAction ("Start conversation", [this] {
		UserSearchDialog::showConversationPicker(backend, treeWidget());

	});

	myMenu.addAction ("Add user to the team", [this] {
		BackendTeam* team = backend.getStorage().getTeamById(teamId);
		if (!team) {
			return;
		}

		FilterListDialogConfig dialogCfg {
			"Add user to team - Mattermost",
			"Search for a user to add to the '" + team->display_name + "' team:",
			"Search users by name:",
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			""
		};

		UserSearchOptions options;
		options.notInTeamId = team->id;
		auto* dialog = new UserSearchDialog(backend, dialogCfg, options, {}, treeWidget());
		dialog->show();

		QObject::connect(dialog, &UserSearchDialog::accepted, [this, team, dialog] {
			const BackendUser* user = dialog->getSelectedUser();
			if (!user) {
				return;
			}
			backend.addUserToTeam(*team, user->id);
		});
	});

	myMenu.addAction ("View Public Channels", [this] {
		BackendTeam* team = backend.getStorage().getTeamById(teamId);
		if (team) {
			TeamChannelsListDialog::showForTeam(backend, *team, treeWidget());
		}
	});

	myMenu.exec (pos);
}

} /* namespace Mattermost */
