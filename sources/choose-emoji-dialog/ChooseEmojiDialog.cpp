/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of MatterLeast.
 */

#include "ChooseEmojiDialog.h"

#include <utility>
#include <QApplication>
#include <QPalette>
#include <QWindow>
#include <QScopedValueRollback>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "EmojiPickerWidget.h"
#include "backend/Backend.h"

namespace Mattermost {

ChooseEmojiDialog::ChooseEmojiDialog()
    : QDialog(nullptr)
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
    connect(this, &QDialog::rejected, this, &ChooseEmojiDialog::clearRequest);
}

ChooseEmojiDialog::~ChooseEmojiDialog() = default;

void ChooseEmojiDialog::ensurePicker()
{
    if (picker_) {
        return;
    }

    picker_ = new EmojiPickerWidget(this);
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
    QObject::disconnect(backendDestroyed_);
    requesterDestroyed_ = {};
    backendDestroyed_ = {};
    requester_.clear();
    callback_ = {};
    backend_.clear();
}

void ChooseEmojiDialog::showFor(Backend& backend, QWidget& requester,
                               std::function<void(Emoji)> callback)
{
    QWidget* owner = requester.window();
    if (!owner) return;
    static QPointer<ChooseEmojiDialog> instance;
    if (!instance) {
        instance = new ChooseEmojiDialog;
        connect(qApp, &QApplication::aboutToQuit, instance.data(),
                [dialog = instance] { delete dialog.data(); });
    }
    auto* dialog = instance.data();
    if (dialog->opening_) return;
    if (dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        return;
    }

    QScopedValueRollback<bool> opening(dialog->opening_, true);
    dialog->clearRequest();
    dialog->backend_ = &backend;
    dialog->requester_ = &requester;
    dialog->callback_ = std::move(callback);
    dialog->backendDestroyed_ = connect(
        &backend, &QObject::destroyed, dialog, &QDialog::reject);
    dialog->requesterDestroyed_ = connect(
        &requester, &QObject::destroyed, dialog, &QDialog::reject);
    dialog->ensurePicker();
    dialog->picker_->setBackend(&backend);
    dialog->picker_->prepare();
    // Preserve native transient stacking for detached chats and modal
    // status editors, without transferring QWidget ownership.
    owner->winId();
    QPointer<QWindow> transientOwner(owner->windowHandle());
    dialog->winId();
    if (auto* native = dialog->windowHandle())
        native->setTransientParent(transientOwner.data());
    dialog->show();
    if (auto* native = dialog->windowHandle())
        native->setTransientParent(transientOwner.data());
    dialog->picker_->focusSearch();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace Mattermost
