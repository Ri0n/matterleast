/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of MatterLeast.
 */

#include "ChooseEmojiDialog.h"

#include <utility>
#include <QPalette>
#include <QScopedValueRollback>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "EmojiPickerWidget.h"
#include "backend/Backend.h"

namespace Mattermost {

ChooseEmojiDialog::ChooseEmojiDialog(Backend& backend, QWidget* parent)
    : QDialog(parent)
    , backend_(&backend)
{
    setObjectName(QStringLiteral("accountEmojiChooser"));
    setWindowTitle(tr("Choose Emoji - Mattermost"));
    setAttribute(Qt::WA_QuitOnClose, false);
    setModal(true);
    resize(483, 345);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    connect(&backend, &QObject::destroyed, this, [this] { delete this; });
    connect(this, &QDialog::rejected, this, &ChooseEmojiDialog::clearRequest);
}

ChooseEmojiDialog::~ChooseEmojiDialog() = default;

void ChooseEmojiDialog::ensurePicker()
{
    if (picker_) {
        return;
    }

    picker_ = new EmojiPickerWidget(*backend_, this);
    layout()->addWidget(picker_);

    connect(picker_, &EmojiPickerWidget::emojiChosen,
            this, [this](const Emoji& emoji) {
        // Complete the request before publishing the callback. Its owner
        // may close or replace the originating message/editor synchronously.
        auto callback = std::move(callback_);
        const bool valid = backend_ && requester_;
        clearRequest();
        accept();
        if (valid && callback)
            callback(emoji);
    });
}

void ChooseEmojiDialog::clearRequest()
{
    QObject::disconnect(requesterDestroyed_);
    requesterDestroyed_ = {};
    requester_.clear();
    callback_ = {};
}

void ChooseEmojiDialog::showFor(Backend& backend, QWidget& requester,
                               std::function<void(Emoji)> callback)
{
    QWidget* owner = requester.window();
    if (!owner) return;
    auto* dialog = owner->findChild<ChooseEmojiDialog*>(
        QStringLiteral("accountEmojiChooser"), Qt::FindDirectChildrenOnly);
    if (dialog && dialog->opening_)
        return;
    if (dialog && dialog->backend_ != &backend) {
        // The registry and picker contents belong to the live connection.
        // Retire the old picker instead of changing its Backend reference.
        dialog->reject();
        dialog->setObjectName(QString());
        dialog->deleteLater();
        dialog = nullptr;
    }
    if (!dialog)
        dialog = new ChooseEmojiDialog(backend, owner);
    if (dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        return;
    }

    QScopedValueRollback<bool> opening(dialog->opening_, true);
    dialog->clearRequest();
    dialog->requester_ = &requester;
    dialog->callback_ = std::move(callback);
    dialog->requesterDestroyed_ = connect(
        &requester, &QObject::destroyed, dialog, &QDialog::reject);
    dialog->ensurePicker();
    dialog->picker_->prepare();
    dialog->show();
    dialog->picker_->focusSearch();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace Mattermost
