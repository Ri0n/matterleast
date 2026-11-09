#include "mainwindow.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QToolTip>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_mainwindow.h"
#include "backend/Backend.h"
#include "backend/CustomStatusValidation.h"
#include "backend/types/BackendUser.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "info-dialogs/UserProfileDialog.h"
#include "ui/EmojiPresentation.h"

namespace Mattermost {
namespace {

void reportStatusResult(QPointer<MainWindow> window, bool success,
                        const QString& reason)
{
    if (!success && window) {
        // Never start a nested event loop inside a network callback. In
        // particular, a modal QMessageBox could process a WebSocket reconnect
        // while the HTTP connector was still unwinding an aborted reply.
        QWidget* anchor = window->findChild<QWidget*>(
            QStringLiteral("usericon_label"));
        if (!anchor) anchor = window.data();
        QToolTip::showText(anchor->mapToGlobal(QPoint(0, anchor->height())),
                           QObject::tr("Status update failed: %1").arg(reason),
                           anchor, QRect(), 10000);
    }
}

} // namespace

void MainWindow::refreshProfileStatus()
{
    const BackendUser* user = backend.getStorage().loginUser;
    if (!user) {
        return;
    }
    ui->usericon_label->setStatus(user->status);

    if (!availabilityUpdateStatus.isEmpty()) {
        ui->statusLabel->setFont(profileStatusDefaultFont);
        ui->statusLabel->setContentsMargins(0, 0, 0, 0);
        ui->statusLabel->setText(
            tr("Setting %1…").arg(availabilityUpdateLabel));
        return;
    }

    // Restore the original font after a pending availability change, even if
    // the custom status consists solely of a large emoji.
    const auto& custom = user->props.customStatus;
    const bool emojiOnly = isEmojiOnlyCustomStatus(custom.emoji, custom.text);
    ui->statusLabel->setFont(emojiOnly
        ? EmojiPresentation::fontForMode(
              profileStatusDefaultFont, EmojiPresentation::Mode::StatusJumbo)
        : profileStatusDefaultFont);
    ui->statusLabel->setContentsMargins(custom.emoji.isEmpty() ? 0 : 8, 0, 0, 0);
    if (custom.emoji.isEmpty()) {
        ui->statusLabel->setText(
            custom.text.isEmpty() ? user->status : custom.text);
        return;
    }

    const auto presentation = backend.emojiRegistry().resolveByName(custom.emoji);
    QString icon = presentation
        ? presentation->unicodeString
        : QStringLiteral(":%1:").arg(custom.emoji.toHtmlEscaped());
    if (icon.contains(QStringLiteral("<img"), Qt::CaseInsensitive)) {
        icon = EmojiPresentation::normalizeHtml(
            icon, profileStatusDefaultFont,
            emojiOnly ? EmojiPresentation::Mode::StatusJumbo
                      : EmojiPresentation::Mode::Reaction,
            &backend.emojiRegistry());
    } else {
        icon = icon.toHtmlEscaped();
    }
    const QString message = custom.text.isEmpty()
        ? QString()
        : QStringLiteral(" ") + custom.text.toHtmlEscaped();
    ui->statusLabel->setText(QStringLiteral("<span>%1%2</span>").arg(icon, message));
}

void MainWindow::on_usericon_label_clicked()
{
    // A late network recovery or session reset can temporarily leave the
    // profile widget alive without a logged-in user. getLoginUser()
    // dereferences that pointer, so inspect it before opening the menu.
    const BackendUser* userPtr = backend.getStorage().loginUser;
    if (!userPtr || userPtr->id.isEmpty()) {
        return;
    }
    const BackendUser& user = *userPtr;

    QMenu menu(this);
    auto* title = menu.addAction(availabilityUpdateStatus.isEmpty()
        ? tr("Availability")
        : tr("Setting availability to %1…").arg(availabilityUpdateLabel));
    title->setEnabled(false);

    QActionGroup availability(&menu);
    availability.setExclusive(true);
    const struct { const char* value; const char* title; } statuses[] = {
        {"online", QT_TR_NOOP("Online")},
        {"away", QT_TR_NOOP("Away")},
        {"dnd", QT_TR_NOOP("Do not disturb")},
        {"offline", QT_TR_NOOP("Offline")},
    };
    for (const auto& option : statuses) {
        const QString value = QString::fromLatin1(option.value);
        const QString label = tr(option.title);
        QAction* item = menu.addAction(label);
        item->setCheckable(true);
        item->setChecked(user.status == value);
        item->setEnabled(availabilityUpdateStatus.isEmpty() && user.status != value);
        availability.addAction(item);
        connect(item, &QAction::triggered, this, [this, value, label] {
            if (!availabilityUpdateStatus.isEmpty()) return;

            availabilityUpdateStatus = value;
            availabilityUpdateLabel = label;
            ui->usericon_label->setAvailabilityUpdateMessage(
                tr("Setting availability to %1…").arg(label));
            refreshProfileStatus();

            QPointer<MainWindow> guard(this);
            backend.setOwnAvailability(value, [guard, value](bool ok,
                                                              const QString& error) {
                if (!guard || guard->availabilityUpdateStatus != value) return;

                guard->availabilityUpdateStatus.clear();
                guard->availabilityUpdateLabel.clear();
                guard->ui->usericon_label->setAvailabilityUpdateMessage(QString());
                guard->refreshProfileStatus();
                reportStatusResult(guard, ok, error);
            });
        });
    }

    menu.addSeparator();
    const QString currentCustom = user.props.customStatus.text;
    const QString currentEmoji = user.props.customStatus.emoji;
    const bool hasCustomStatus = hasCustomStatusContent(currentEmoji, currentCustom);
    if (hasCustomStatus) {
        const QString summary = currentCustom.isEmpty()
            ? tr(":%1:").arg(currentEmoji) : currentCustom;
        QAction* current = menu.addAction(tr("Custom: %1").arg(summary));
        current->setEnabled(false);
    }

    QAction* setCustom = menu.addAction(tr("Set a custom status…"));
    connect(setCustom, &QAction::triggered, this, [this] {
        QDialog editor(this);
        editor.setObjectName(QStringLiteral("customStatusEditor"));
        editor.setWindowTitle(tr("Set a custom status"));
        auto* form = new QFormLayout(&editor);

        auto* textEdit = new QLineEdit(&editor);
        textEdit->setObjectName(QStringLiteral("customStatusText"));
        textEdit->setMaxLength(100);
        textEdit->setPlaceholderText(tr("What's your status?"));
        textEdit->setText(backend.getLoginUser().props.customStatus.text);
        form->addRow(tr("Status message:"), textEdit);

        auto* emojiRow = new QWidget(&editor);
        auto* emojiLayout = new QHBoxLayout(emojiRow);
        emojiLayout->setContentsMargins(0, 0, 0, 0);
        auto* emojiName = new QLineEdit(emojiRow);
        emojiName->setObjectName(QStringLiteral("customStatusEmoji"));
        emojiName->setPlaceholderText(QStringLiteral("speech_balloon"));
        emojiName->setText(backend.getLoginUser().props.customStatus.emoji);
        emojiLayout->addWidget(emojiName, 1);
        auto* choose = new QPushButton(tr("Choose…"), emojiRow);
        emojiLayout->addWidget(choose);
        connect(choose, &QPushButton::clicked, &editor, [emojiName] {
            QPointer<QLineEdit> target(emojiName);
            showEmojiDialog([target](const Emoji& emoji) {
                if (target) target->setText(emoji.name);
            });
        });
        form->addRow(tr("Emoji:"), emojiRow);

        auto* duration = new QComboBox(&editor);
        duration->setObjectName(QStringLiteral("customStatusDuration"));
        duration->addItem(tr("Don't clear automatically"), QString());
        duration->addItem(tr("After 30 minutes"), QStringLiteral("thirty_minutes"));
        duration->addItem(tr("After 1 hour"), QStringLiteral("one_hour"));
        duration->addItem(tr("After 4 hours"), QStringLiteral("four_hours"));
        duration->addItem(tr("Today"), QStringLiteral("today"));
        duration->addItem(tr("This week"), QStringLiteral("this_week"));
        duration->addItem(tr("Choose date and time"), QStringLiteral("date_and_time"));
        const QString previousDuration = backend.getLoginUser().props.customStatus.duration;
        const int index = duration->findData(previousDuration);
        duration->setCurrentIndex(index < 0 ? 0 : index);
        form->addRow(tr("Clear status:"), duration);

        auto* expiration = new QDateTimeEdit(QDateTime::currentDateTime().addDays(1), &editor);
        expiration->setObjectName(QStringLiteral("customStatusExpiresAt"));
        expiration->setCalendarPopup(true);
        expiration->setMinimumDateTime(QDateTime::currentDateTime());
        expiration->setVisible(duration->currentData().toString() ==
                               QLatin1String("date_and_time"));
        form->addRow(tr("Clear on:"), expiration);
        connect(duration, qOverload<int>(&QComboBox::currentIndexChanged),
                &editor, [duration, expiration] {
            expiration->setVisible(duration->currentData().toString()
                == QLatin1String("date_and_time"));
        });

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &editor);
        // OK with an emoji only saves that emoji. OK with both fields empty
        // explicitly clears the previous custom status; neither may leave
        // the dialog unresponsive.
        connect(buttons, &QDialogButtonBox::accepted, &editor, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        form->addRow(buttons);

        if (editor.exec() != QDialog::Accepted) return;
        const QString durationValue = duration->currentData().toString();
        const QString expiresAt = durationValue == QLatin1String("date_and_time")
            ? expiration->dateTime().toUTC().toString(Qt::ISODate) : QString();
        QPointer<MainWindow> guard(this);
        const QString emoji = emojiName->text().trimmed();
        const QString message = textEdit->text().trimmed();
        auto finished = [guard](bool ok, const QString& error) {
            reportStatusResult(guard, ok, error);
        };
        if (!hasCustomStatusContent(emoji, message)) {
            backend.clearOwnCustomStatus(finished);
        } else {
            backend.setOwnCustomStatus(emoji, message, durationValue, expiresAt, finished);
        }
    });

    QAction* clear = menu.addAction(tr("Clear custom status"));
    clear->setEnabled(hasCustomStatus);
    connect(clear, &QAction::triggered, this, [this] {
        QPointer<MainWindow> guard(this);
        backend.clearOwnCustomStatus([guard](bool ok, const QString& error) {
            reportStatusResult(guard, ok, error);
        });
    });

    menu.addSeparator();
    QAction* profile = menu.addAction(tr("View profile…"));
    connect(profile, &QAction::triggered, this, [this] {
        UserProfileDialog::showTransient(backend, backend.getLoginUser(), this);
    });

    menu.exec(ui->usericon_label->mapToGlobal(QPoint(0, ui->usericon_label->height())));
}

} // namespace Mattermost
