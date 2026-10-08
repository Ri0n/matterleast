/**
 * @file PostAttachmentListWidget.h
 * @brief
 * @author Lyubomir Filipov
 * @date Jan 21, 2022
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

#pragma once

#include <qlistwidget.h>

namespace Mattermost {

class PostAttachmentListWidget: public QListWidget {
public:
	using QListWidget::QListWidget;
	QSize sizeHint () const	override;

protected:
	// The list is only a layout container. Leave mouse input to the enclosing
	// PostWidget so a drag that starts beside an attachment selects messages.
	void mousePressEvent (QMouseEvent* event) override;
	void mouseMoveEvent (QMouseEvent* event) override;
	void mouseReleaseEvent (QMouseEvent* event) override;
	void mouseDoubleClickEvent (QMouseEvent* event) override;
private:
};

} /* namespace Mattermost */
