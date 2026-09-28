/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 */

#include "ChooseEmojiDialog.h"

#include <QPalette>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "EmojiPickerWidget.h"

namespace Mattermost {

ChooseEmojiDialog::ChooseEmojiDialog(Backend& backend, QWidget* parent)
    : QDialog(parent)
    , backend_(backend)
{
    setWindowTitle(tr("Choose Emoji - Mattermost"));
    setModal(true);
    resize(483, 345);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
}

ChooseEmojiDialog::~ChooseEmojiDialog() = default;

void ChooseEmojiDialog::ensurePicker()
{
    if (picker_) {
        return;
    }

    picker_ = new EmojiPickerWidget(backend_, this);
    layout()->addWidget(picker_);

    connect(picker_, &EmojiPickerWidget::emojiChosen,
            this, [this](const Emoji& emoji) {
        selectedEmoji_ = emoji;
        accept();
    });
}

Emoji ChooseEmojiDialog::getSelectedEmoji()
{
    return selectedEmoji_;
}

void ChooseEmojiDialog::show()
{
    ensurePicker();
    picker_->prepare();

    QDialog::show();
    picker_->focusSearch();
}

} // namespace Mattermost
