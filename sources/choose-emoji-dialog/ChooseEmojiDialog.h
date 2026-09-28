
/**
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

#pragma once

#include <QDialog>

#include "backend/emoji/EmojiDefs.h"

namespace Mattermost {

class Backend;
class EmojiPickerWidget;

class ChooseEmojiDialog : public QDialog
{
    Q_OBJECT

private:
    explicit ChooseEmojiDialog(Backend& backend, QWidget* parent = nullptr);
    ~ChooseEmojiDialog() override;

public:
    void show();

private:
    friend class ChooseEmojiDialogWrapper;

    void ensurePicker();
    Emoji getSelectedEmoji();

    Backend& backend_;
    EmojiPickerWidget* picker_ = nullptr;
    Emoji selectedEmoji_;
};

} // namespace Mattermost
