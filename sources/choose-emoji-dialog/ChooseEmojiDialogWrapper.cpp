/**
 * @file ChooseEmojiDialogWrapper.cpp
 * @brief Shows the ChooseEmojiDialog with given callback for chosen emoji
 * @author Lyubomir Filipov
 * @date Jan 2, 2023
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

#include "ChooseEmojiDialogWrapper.h"
#include <QDebug>
#include <QPointer>
#include <QScopedValueRollback>
#include <QWindow>

namespace Mattermost {

static ChooseEmojiDialog* gChooseEmojiDialog;
static std::function<void (Emoji)> chooseEmojiCallback;

ChooseEmojiDialogWrapper::ChooseEmojiDialogWrapper (QWidget* parent, Backend& backend)
:dialog (backend, parent)
{
	gChooseEmojiDialog = &dialog;

	QObject::connect (gChooseEmojiDialog, &QDialog::accepted, [] {
		if (!chooseEmojiCallback) {
			qCritical() << "ChooseEmojiDialog::accepted() called without active chooseEmojiCallback";
			return;
		}
		chooseEmojiCallback (gChooseEmojiDialog->getSelectedEmoji ());
	});

}

ChooseEmojiDialogWrapper::~ChooseEmojiDialogWrapper () = default;

void showEmojiDialog(std::function<void(Emoji)> chooseCallback, QWidget* requester)
{
    if (!gChooseEmojiDialog)
        return;

    // A cold picker performs one-time setup before it is shown. Prevent
    // repeated triggers during that setup from replacing the initial callback
    // or opening the same picker a second time.
    static bool opening = false;
    if (opening)
        return;
    if (gChooseEmojiDialog->isVisible()) {
        gChooseEmojiDialog->raise();
        gChooseEmojiDialog->activateWindow();
        return;
    }
    QScopedValueRollback<bool> openingGuard(opening, true);
    chooseEmojiCallback = std::move(chooseCallback);

    // The picker is reused globally, but when invoked from a nested modal
    // editor it must be transient for that editor rather than just its main
    // window owner; otherwise some window managers stack it underneath.
    // Reparent only the native transient relationship, not QWidget ownership.
    QWidget* parentWindow = requester ? requester->window()
        : gChooseEmojiDialog->parentWidget();
    QPointer<QWindow> transientOwner =
        parentWindow ? parentWindow->windowHandle() : nullptr;
    gChooseEmojiDialog->winId();
    if (QWindow* pickerWindow = gChooseEmojiDialog->windowHandle())
        pickerWindow->setTransientParent(transientOwner.data());
    gChooseEmojiDialog->show();
    // Widget show() can update the native parent from its permanent QWidget
    // parent. Restore the actual requesting modal dialog if that happened.
    if (QWindow* pickerWindow = gChooseEmojiDialog->windowHandle()) {
        if (pickerWindow->transientParent() != transientOwner.data())
            pickerWindow->setTransientParent(transientOwner.data());
    }
    gChooseEmojiDialog->raise();
    gChooseEmojiDialog->activateWindow();
}

} /* namespace Mattermost */
