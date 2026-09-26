#include "CreateChannelDialog.h"

#include <algorithm>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

#include "backend/types/BackendTeam.h"

namespace Mattermost {

CreateChannelDialog::CreateChannelDialog(const BackendTeam& team, QWidget* parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Create channel - %1").arg(team.display_name));
    resize(460, 300);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    displayNameEdit = new QLineEdit(this);
    displayNameEdit->setPlaceholderText(tr("Channel name"));
    form->addRow(tr("Display name:"), displayNameEdit);

    channelNameEdit = new QLineEdit(this);
    channelNameEdit->setPlaceholderText(tr("channel-name"));
    channelNameEdit->setToolTip(
        tr("URL name: lowercase letters, numbers, '-' and '_'"));
    form->addRow(tr("URL name:"), channelNameEdit);

    typeCombo = new QComboBox(this);
    typeCombo->addItem(tr("Public"), QStringLiteral("O"));
    typeCombo->addItem(tr("Private"), QStringLiteral("P"));
    form->addRow(tr("Type:"), typeCombo);

    purposeEdit = new QTextEdit(this);
    purposeEdit->setAcceptRichText(false);
    purposeEdit->setPlaceholderText(tr("Optional short description"));
    purposeEdit->setMaximumHeight(90);
    form->addRow(tr("Purpose:"), purposeEdit);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted,
            this, &CreateChannelDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    connect(channelNameEdit, &QLineEdit::textEdited, this,
            [this] { channelNameEdited = true; });
    connect(displayNameEdit, &QLineEdit::textChanged, this,
            [this] {
        if (!channelNameEdited) {
            updateGeneratedName();
        }
    });
}

QString CreateChannelDialog::generatedChannelName(const QString& displayName)
{
    QString result;
    bool pendingSeparator = false;
    const QString lower = displayName.toLower();

    for (const QChar ch : lower) {
        const ushort u = ch.unicode();
        const bool asciiLetter = u >= 'a' && u <= 'z';
        const bool digit = u >= '0' && u <= '9';
        if (asciiLetter || digit) {
            if (pendingSeparator && !result.isEmpty()
                && !result.endsWith(QLatin1Char('-'))) {
                result += QLatin1Char('-');
            }
            pendingSeparator = false;
            result += ch;
        } else if (!result.isEmpty()) {
            pendingSeparator = true;
        }
        if (result.size() >= 64) {
            break;
        }
    }

    while (result.endsWith(QLatin1Char('-'))) {
        result.chop(1);
    }
    if (result.isEmpty() && !displayName.trimmed().isEmpty()) {
        result = QStringLiteral("channel-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    }
    return result.left(64);
}

void CreateChannelDialog::updateGeneratedName()
{
    channelNameEdit->setText(generatedChannelName(displayNameEdit->text()));
}

QString CreateChannelDialog::channelName() const
{
    return channelNameEdit->text().trimmed();
}

QString CreateChannelDialog::displayName() const
{
    return displayNameEdit->text().trimmed();
}

QString CreateChannelDialog::purpose() const
{
    return purposeEdit->toPlainText().trimmed();
}

bool CreateChannelDialog::isPrivateChannel() const
{
    return typeCombo->currentData().toString() == QStringLiteral("P");
}

void CreateChannelDialog::accept()
{
    if (displayName().isEmpty()) {
        QMessageBox::warning(this, tr("Create channel"),
                             tr("Display name cannot be empty."));
        displayNameEdit->setFocus();
        return;
    }

    static const QRegularExpression validName(
        QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
    if (!validName.match(channelName()).hasMatch()) {
        QMessageBox::warning(
            this, tr("Create channel"),
            tr("URL name must contain only lowercase letters, numbers, '-' "
               "or '_', start with a letter or number, and be at most 64 "
               "characters long."));
        channelNameEdit->setFocus();
        channelNameEdit->selectAll();
        return;
    }

    QDialog::accept();
}

} // namespace Mattermost
