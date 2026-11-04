#include "ChannelTopicEditDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "backend/Backend.h"
#include "backend/ChannelTopicPolicy.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

ChannelTopicEditDialog::ChannelTopicEditDialog(
    Backend& backend, BackendChannel& channel, QWidget* parent)
    : QDialog(parent), _backend(backend), _channel(&channel),
      _originalHeader(channel.header)
{
    setObjectName(QStringLiteral("channelTopicEditDialog"));
    setWindowTitle(tr("Edit channel topic"));
    setMinimumWidth(440);
    setWindowModality(Qt::WindowModal);

    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(
        tr("Channel topic (Markdown, maximum 1024 characters)"), this);
    layout->addWidget(description);

    _editor = new QPlainTextEdit(this);
    _editor->setObjectName(QStringLiteral("channelTopicEditor"));
    _editor->setPlainText(_originalHeader);
    _editor->setMinimumHeight(130);
    _editor->setTabChangesFocus(true);
    layout->addWidget(_editor);

    _counter = new QLabel(this);
    _counter->setObjectName(QStringLiteral("channelTopicCounter"));
    layout->addWidget(_counter);

    _error = new QLabel(this);
    _error->setObjectName(QStringLiteral("channelTopicError"));
    _error->setWordWrap(true);
    _error->hide();
    layout->addWidget(_error);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    _save = buttons->button(QDialogButtonBox::Save);
    _save->setObjectName(QStringLiteral("channelTopicSave"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(_save, &QPushButton::clicked, this,
            &ChannelTopicEditDialog::save);
    connect(_editor, &QPlainTextEdit::textChanged, this,
            &ChannelTopicEditDialog::refreshValidation);
    connect(_channel, &QObject::destroyed, this, &QDialog::reject);
    refreshValidation();
}

void ChannelTopicEditDialog::refreshValidation()
{
    const QString text = _editor->toPlainText();
    const int length = text.size();
    _counter->setText(tr("%1 / %2 characters")
                      .arg(length).arg(ChannelTopicPolicy::MaximumLength));
    _counter->setToolTip(length > ChannelTopicPolicy::MaximumLength
        ? tr("The topic exceeds the server's 1024-character limit.")
        : QString());
    _save->setEnabled(!_saving && _channel
        && ChannelTopicPolicy::valid(text)
        && ChannelTopicPolicy::changed(_originalHeader, text));
    if (!_saving) _error->hide();
}

void ChannelTopicEditDialog::save()
{
    if (!_channel || _saving) return;
    const QString text = _editor->toPlainText();
    if (!ChannelTopicPolicy::valid(text)
        || !ChannelTopicPolicy::changed(_originalHeader, text)) {
        return;
    }

    _saving = true;
    _save->setEnabled(false);
    _editor->setReadOnly(true);
    QPointer<ChannelTopicEditDialog> guard(this);
    _backend.editChannelHeader(*_channel, text,
        [guard](bool ok, const QString& error) {
        if (!guard) return;
        guard->_saving = false;
        guard->_editor->setReadOnly(false);
        if (ok) {
            guard->accept();
        } else {
            guard->refreshValidation();
            guard->_error->setText(error);
            guard->_error->show();
        }
    });
}

} // namespace Mattermost
