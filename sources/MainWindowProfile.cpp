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
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_mainwindow.h"
#include "backend/Backend.h"
#include "backend/types/BackendUser.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "info-dialogs/UserProfileDialog.h"

namespace Mattermost {
namespace {

void reportStatusResult(QPointer<MainWindow> window, bool success,
                        const QString& reason)
{
    if (!success && window) {
        QMessageBox::warning(window, QObject::tr("Status update failed"), reason);
    }
}

} // namespace

void MainWindow::on_usericon_label_clicked()
{
    const BackendUser& user = backend.getLoginUser();
    if (user.id.isEmpty()) {
        return;
    }

    QMenu menu(this);
    auto* title = menu.addAction(tr("Availability"));
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
        QAction* item = menu.addAction(tr(option.title));
        item->setCheckable(true);
        item->setChecked(user.status == value);
        availability.addAction(item);
        connect(item, &QAction::triggered, this, [this, value] {
            QPointer<MainWindow> guard(this);
            backend.setOwnAvailability(value, [guard](bool ok, const QString& error) {
                reportStatusResult(guard, ok, error);
            });
        });
    }

    menu.addSeparator();
    const QString currentCustom = user.props.customStatus.text;
    if (!currentCustom.isEmpty()) {
        QAction* current = menu.addAction(
            tr("Custom: %1").arg(currentCustom));
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
        connect(buttons, &QDialogButtonBox::accepted, &editor, [&editor, textEdit] {
            if (!textEdit->text().trimmed().isEmpty()) editor.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
        form->addRow(buttons);

        if (editor.exec() != QDialog::Accepted) return;
        const QString durationValue = duration->currentData().toString();
        const QString expiresAt = durationValue == QLatin1String("date_and_time")
            ? expiration->dateTime().toUTC().toString(Qt::ISODate) : QString();
        QPointer<MainWindow> guard(this);
        backend.setOwnCustomStatus(
            emojiName->text().trimmed(), textEdit->text().trimmed(),
            durationValue, expiresAt,
            [guard](bool ok, const QString& error) {
                reportStatusResult(guard, ok, error);
            });
    });

    QAction* clear = menu.addAction(tr("Clear custom status"));
    clear->setEnabled(!currentCustom.isEmpty());
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
