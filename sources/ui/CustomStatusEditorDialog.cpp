#include "CustomStatusEditorDialog.h"

#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "backend/CustomStatusValidation.h"
#include "backend/emoji/EmojiRegistry.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "ui/UserStatusEmoji.h"

namespace Mattermost {
namespace {

QString durationLabel(const QString& duration)
{
    if (duration == QLatin1String("thirty_minutes"))
        return QObject::tr("30 minutes");
    if (duration == QLatin1String("one_hour"))
        return QObject::tr("1 hour");
    if (duration == QLatin1String("four_hours"))
        return QObject::tr("4 hours");
    if (duration == QLatin1String("today"))
        return QObject::tr("Today");
    if (duration == QLatin1String("this_week"))
        return QObject::tr("This week");
    return {};
}

} // namespace

CustomStatusEditorDialog::CustomStatusEditorDialog(
    EmojiRegistry& registry, const BackendUserCustomStatus& current, QWidget* parent)
    : QDialog(parent), m_registry(registry), m_initial(current)
{
    setObjectName(QStringLiteral("customStatusEditor"));
    setWindowTitle(tr("Set a custom status"));
    resize(450, 450);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    root->addLayout(form);

    m_text = new QLineEdit(this);
    m_text->setObjectName(QStringLiteral("customStatusText"));
    m_text->setMaxLength(100);
    m_text->setPlaceholderText(tr("What's your status?"));
    m_text->setText(current.text);
    form->addRow(tr("Status message:"), m_text);

    auto* emojiRow = new QWidget(this);
    auto* emojiLayout = new QHBoxLayout(emojiRow);
    emojiLayout->setContentsMargins(0, 0, 0, 0);
    m_emoji = new QLineEdit(emojiRow);
    m_emoji->setObjectName(QStringLiteral("customStatusEmoji"));
    m_emoji->setPlaceholderText(QStringLiteral("speech_balloon"));
    m_emoji->setText(current.emoji);
    emojiLayout->addWidget(m_emoji, 1);
    auto* choose = new QPushButton(tr("Choose…"), emojiRow);
    emojiLayout->addWidget(choose);
    connect(choose, &QPushButton::clicked, this, [this] {
        QPointer<QLineEdit> target(m_emoji);
        showEmojiDialog([target](const Emoji& emoji) {
            if (target)
                target->setText(emoji.name);
        });
    });
    form->addRow(tr("Emoji:"), emojiRow);

    m_duration = new QComboBox(this);
    m_duration->setObjectName(QStringLiteral("customStatusDuration"));
    m_duration->addItem(tr("Don't clear automatically"), QString());
    m_duration->addItem(tr("After 30 minutes"), QStringLiteral("thirty_minutes"));
    m_duration->addItem(tr("After 1 hour"), QStringLiteral("one_hour"));
    m_duration->addItem(tr("After 4 hours"), QStringLiteral("four_hours"));
    m_duration->addItem(tr("Today"), QStringLiteral("today"));
    m_duration->addItem(tr("This week"), QStringLiteral("this_week"));
    m_duration->addItem(tr("Choose date and time"), QStringLiteral("date_and_time"));
    const int index = m_duration->findData(current.duration);
    m_duration->setCurrentIndex(index < 0 ? 0 : index);
    form->addRow(tr("Clear status:"), m_duration);

    QDateTime when = QDateTime::fromString(current.expiresAt, Qt::ISODate);
    if (!when.isValid() || when <= QDateTime::currentDateTime())
        when = QDateTime::currentDateTime().addDays(1);
    m_expiration = new QDateTimeEdit(when.toLocalTime(), this);
    m_expiration->setObjectName(QStringLiteral("customStatusExpiresAt"));
    m_expiration->setCalendarPopup(true);
    m_expiration->setMinimumDateTime(QDateTime::currentDateTime());
    m_expiration->setVisible(m_duration->currentData().toString()
                             == QLatin1String("date_and_time"));
    form->addRow(tr("Clear on:"), m_expiration);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("customStatusSuggestionsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMaximumHeight(265);
    root->addWidget(scroll, 1);
    m_suggestionSection = new QWidget(scroll);
    auto* suggestions = new QVBoxLayout(m_suggestionSection);
    suggestions->setContentsMargins(0, 0, 0, 0);
    suggestions->setSpacing(1);
    scroll->setWidget(m_suggestionSection);

    m_recentSection = new QWidget(m_suggestionSection);
    auto* recentLayout = new QVBoxLayout(m_recentSection);
    recentLayout->setContentsMargins(0, 0, 0, 0);
    recentLayout->setSpacing(1);
    auto* recentTitle = new QLabel(tr("RECENT"), m_recentSection);
    recentLayout->addWidget(recentTitle);
    m_recentRows = new QVBoxLayout;
    m_recentRows->setSpacing(0);
    recentLayout->addLayout(m_recentRows);
    m_recentSection->hide();
    suggestions->addWidget(m_recentSection);

    auto* presetTitle = new QLabel(tr("SUGGESTIONS"), m_suggestionSection);
    suggestions->addWidget(presetTitle);
    const struct {
        const char* id;
        const char* emoji;
        QString text;
        const char* duration;
    } presets[] = {
        {"Meeting", "calendar", tr("In a meeting"), "one_hour"},
        {"Lunch", "hamburger", tr("Out for lunch"), "thirty_minutes"},
        {"Sick", "sneezing_face", tr("Out sick"), "today"},
        {"Home", "house", tr("Working from home"), "today"},
        {"Vacation", "palm_tree", tr("On a vacation"), "this_week"}
    };
    for (const auto& preset : presets) {
        BackendUserCustomStatus status;
        status.emoji = QString::fromLatin1(preset.emoji);
        status.text = preset.text;
        status.duration = QString::fromLatin1(preset.duration);
        auto* button = createSuggestionButton(status,
            QStringLiteral("customStatusPreset") + QString::fromLatin1(preset.id),
            m_suggestionSection);
        m_presets.push_back(button);
        suggestions->addWidget(button);
        connect(button, &QPushButton::clicked, this,
                [this, status] { applySuggestion(status); });
    }
    suggestions->addStretch();

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_duration, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
        m_expiration->setVisible(statusDuration() == QLatin1String("date_and_time"));
        updateSuggestionVisibility();
    });
    connect(m_text, &QLineEdit::textChanged, this,
            [this] { updateSuggestionVisibility(); });
    connect(m_emoji, &QLineEdit::textChanged, this,
            [this] { updateSuggestionVisibility(); });
    updateSuggestionVisibility();
}

QString CustomStatusEditorDialog::statusEmoji() const
{
    return m_emoji->text().trimmed();
}

QString CustomStatusEditorDialog::statusText() const
{
    return m_text->text().trimmed();
}

QString CustomStatusEditorDialog::statusDuration() const
{
    return m_duration->currentData().toString();
}

QString CustomStatusEditorDialog::statusExpiresAt() const
{
    return statusDuration() == QLatin1String("date_and_time")
        ? m_expiration->dateTime().toUTC().toString(Qt::ISODate) : QString();
}

void CustomStatusEditorDialog::applySuggestion(const BackendUserCustomStatus& status)
{
    m_emoji->setText(status.emoji);
    m_text->setText(status.text);
    const int index = m_duration->findData(status.duration);
    m_duration->setCurrentIndex(index < 0 ? 0 : index);
    if (status.duration == QLatin1String("date_and_time")) {
        const QDateTime expires = QDateTime::fromString(status.expiresAt, Qt::ISODate);
        if (expires.isValid() && expires > QDateTime::currentDateTime())
            m_expiration->setDateTime(expires.toLocalTime());
    }
}

QPushButton* CustomStatusEditorDialog::createSuggestionButton(
    const BackendUserCustomStatus& status, const QString& objectName, QWidget* parent)
{
    auto* button = new QPushButton(parent);
    button->setObjectName(objectName);
    button->setFlat(true);
    button->setMinimumHeight(36);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(button);
    layout->setContentsMargins(8, 3, 8, 3);
    layout->setSpacing(10);

    auto* emoji = new QLabel(button);
    emoji->setAttribute(Qt::WA_TransparentForMouseEvents);
    emoji->setFixedSize(24, 24);
    const QPixmap icon = UserStatusEmoji::pixmap(m_registry, status.emoji, 22, font());
    if (!icon.isNull())
        emoji->setPixmap(icon);
    else
        emoji->setText(QStringLiteral(":%1:").arg(status.emoji));
    layout->addWidget(emoji);

    auto* label = new QLabel(status.text, button);
    label->setTextFormat(Qt::PlainText);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label, 1);
    const QString hint = durationLabel(status.duration);
    if (!hint.isEmpty()) {
        auto* duration = new QLabel(hint, button);
        duration->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto colors = duration->palette();
        colors.setColor(QPalette::WindowText,
                        colors.color(QPalette::Disabled, QPalette::WindowText));
        duration->setPalette(colors);
        layout->addWidget(duration);
    }
    button->setAccessibleName(hint.isEmpty()
        ? status.text : status.text + QStringLiteral(", ") + hint);
    return button;
}

void CustomStatusEditorDialog::setRemoveRecentCallback(RemoveRecent callback)
{
    m_removeRecent = std::move(callback);
}

void CustomStatusEditorDialog::setRecentStatuses(
    const QVector<BackendUserCustomStatus>& statuses)
{
    m_recentStatuses.clear();
    while (QLayoutItem* item = m_recentRows->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto& status : statuses) {
        if (!hasCustomStatusContent(status.emoji, status.text))
            continue;
        const int rowIndex = m_recentStatuses.size();
        m_recentStatuses.push_back(status);
        auto* row = new QWidget(m_recentSection);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto* choice = createSuggestionButton(status,
            QStringLiteral("customStatusRecent%1").arg(rowIndex), row);
        layout->addWidget(choice, 1);
        connect(choice, &QPushButton::clicked, this,
                [this, status] { applySuggestion(status); });
        if (m_removeRecent) {
            auto* remove = new QToolButton(row);
            remove->setObjectName(QStringLiteral("customStatusRemoveRecent%1").arg(rowIndex));
            remove->setText(QStringLiteral("×"));
            remove->setToolTip(tr("Remove from recent statuses"));
            remove->setAutoRaise(true);
            remove->setFixedWidth(28);
            layout->addWidget(remove);
            connect(remove, &QToolButton::clicked, this, [this, status] {
                QPointer<CustomStatusEditorDialog> guard(this);
                m_removeRecent(status, [guard, status](bool ok) {
                    if (!guard || !ok)
                        return;
                    auto remaining = guard->m_recentStatuses;
                    for (auto it = remaining.begin(); it != remaining.end(); ++it) {
                        if (it->emoji == status.emoji && it->text == status.text) {
                            remaining.erase(it);
                            break;
                        }
                    }
                    guard->setRecentStatuses(remaining);
                });
            });
        }
        m_recentRows->addWidget(row);
    }
    m_recentSection->setVisible(!m_recentStatuses.isEmpty());
    updateSuggestionVisibility();
}

void CustomStatusEditorDialog::updateSuggestionVisibility()
{
    const bool empty = !hasCustomStatusContent(statusEmoji(), statusText());
    const QString initialDuration = m_initial.duration == QLatin1String("dont_clear")
        ? QString() : m_initial.duration;
    const bool unchanged = statusEmoji() == m_initial.emoji
        && statusText() == m_initial.text
        && statusDuration() == initialDuration;
    m_suggestionSection->parentWidget()->setVisible(empty || unchanged);

    // The web client avoids repeating a recently used status in suggestions.
    for (QPushButton* preset : m_presets) {
        bool duplicate = false;
        for (const auto& recent : m_recentStatuses) {
            if (preset->accessibleName().startsWith(recent.text + QStringLiteral(","))) {
                duplicate = true;
                break;
            }
        }
        preset->setVisible(!duplicate);
    }
}

} // namespace Mattermost
