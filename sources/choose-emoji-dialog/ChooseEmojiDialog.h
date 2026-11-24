
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

#include <functional>
#include <QDialog>
#include <QPointer>

#include "backend/emoji/EmojiDefs.h"

namespace Mattermost {

class Backend;
class EmojiPickerWidget;

class ChooseEmojiDialog : public QDialog
{
    Q_OBJECT

public:
    // Lazily reuse one picker in the requesting top-level window, never a
    // process-global picker belonging to the last constructed MainWindow.
    static void showFor(Backend& backend, QWidget& requester,
                        std::function<void(Emoji)> callback);

private:
    explicit ChooseEmojiDialog(Backend& backend, QWidget* parent);
    ~ChooseEmojiDialog() override;
    void ensurePicker();
    void clearRequest();

    QPointer<Backend> backend_;
    QPointer<QWidget> requester_;
    QMetaObject::Connection requesterDestroyed_;
    std::function<void(Emoji)> callback_;
    EmojiPickerWidget* picker_ = nullptr;
    bool opening_ = false;
};

} // namespace Mattermost
