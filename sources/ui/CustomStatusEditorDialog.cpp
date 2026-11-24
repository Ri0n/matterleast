#include "CustomStatusEditorDialog.h"

#include <utility>

#include <QAction>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPainter>
#include <QProxyStyle>
#include <QStyle>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include "backend/CustomStatusValidation.h"
#include "backend/emoji/EmojiRegistry.h"
#include "choose-emoji-dialog/ChooseEmojiDialog.h"
#include "backend/Backend.h"
#include "ui/UserStatusEmoji.h"
#include "ui/OverlayScrollBarManager.h"

namespace Mattermost {
namespace {

// QLineEdit's side action is a QToolButton whose geometry is derived from
// PM_LineEditIconSize. Keep that metric local to the status input so the icon
// can be legible without manually moving an internal Qt child widget.
class StatusLineEditStyle final : public QProxyStyle
{
public:
    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (metric == QStyle::PM_LineEditIconSize)
            return 30;
#else
        // Qt 5 uses PM_SmallIconSize for QLineEdit side actions.
        if (metric == QStyle::PM_SmallIconSize)
            return 30;
#endif
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
};

// QLineEdit owns the clickable leading QAction; the separator is drawn
// together with its icon, avoiding hard-coded overlay widget geometry.
QIcon statusEmojiActionIcon(EmojiRegistry& registry, const QString& name,
                            const QPalette& palette, const QFont& font)
{
    QPixmap image(32, 32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QPixmap emoji = UserStatusEmoji::pixmap(registry, name, 27, font);
    if (!emoji.isNull()) {
        painter.drawPixmap(1, 2, emoji);
    } else {
        // Empty or not-yet-resolved emoji: keep the picker accessible with
        // a neutral, non-wire-value placeholder.
        const QColor muted = palette.color(QPalette::Disabled, QPalette::Text);
        painter.setPen(QPen(muted, 1.4));
        painter.drawEllipse(QRectF(3, 4, 25, 25));
        painter.drawLine(QPointF(10, 16), QPointF(21, 16));
        painter.drawLine(QPointF(15.5, 10.5), QPointF(15.5, 21.5));
    }
    painter.setPen(QPen(palette.color(QPalette::Mid), 1.0));
    painter.drawLine(QPointF(30, 3), QPointF(30, 29));
    return QIcon(image);
}

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
    Backend& backend, const BackendUserCustomStatus& current, QWidget* parent)
    : QDialog(parent), m_backend(backend), m_registry(backend.emojiRegistry())
{
    setObjectName(QStringLiteral("customStatusEditor"));
    setWindowTitle(tr("Set a custom status"));
    resize(520, 550);
    setMinimumWidth(470);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 18, 22, 18);
    root->setSpacing(14);
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
    root->addLayout(form);

    m_text = new QLineEdit(this);
    m_text->setObjectName(QStringLiteral("customStatusText"));
    auto* emojiStyle = new StatusLineEditStyle;
    emojiStyle->setParent(m_text);
    m_text->setStyle(emojiStyle);
    m_text->setMinimumHeight(42);
    m_text->setMaxLength(100);
    m_text->setPlaceholderText(tr("What's your status?"));
    m_text->setText(current.text);
    m_selectedEmoji = current.emoji.trimmed();
    m_emojiAction = m_text->addAction(QIcon(), QLineEdit::LeadingPosition);
    m_emojiAction->setObjectName(QStringLiteral("customStatusEmojiAction"));
    // Qt internally represents a QLineEdit QAction with a QToolButton.
    // Only customize the button belonging to this action (not clear actions).
    for (QToolButton* button : m_text->findChildren<QToolButton*>()) {
        if (button->defaultAction() == m_emojiAction) {
            button->setObjectName(QStringLiteral("customStatusEmojiButton"));
            button->setCursor(Qt::PointingHandCursor);
            break;
        }
    }
    m_text->installEventFilter(this);
    connect(m_emojiAction, &QAction::triggered, this, [this] {
        QPointer<CustomStatusEditorDialog> guard(this);
        ChooseEmojiDialog::showFor(m_backend, *this, [guard](const Emoji& emoji) {
            if (guard)
                guard->setSelectedEmoji(emoji.name);
        });
    });
    connect(&m_registry, &EmojiRegistry::customEmojiAdded, this,
            [this](const QString& name) {
        if (name == m_selectedEmoji)
            updateEmojiAction();
    });
    updateEmojiAction();
    form->addRow(tr("Status message:"), m_text);

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
    const bool showExpiration = m_duration->currentData().toString()
        == QLatin1String("date_and_time");
    m_expiration->setVisible(showExpiration);
    m_expirationLabel = new QLabel(tr("Clear on:"), this);
    m_expirationLabel->setObjectName(QStringLiteral("customStatusExpiresAtLabel"));
    m_expirationLabel->setVisible(showExpiration);
    form->addRow(m_expirationLabel, m_expiration);

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("customStatusSuggestionsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMaximumHeight(340);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    OverlayScrollBarManager::setEdgeNavigationButtonsEnabled(*scroll, false);
    root->addWidget(scroll, 1);
    m_suggestionSection = new QWidget(scroll);
    auto* suggestions = new QVBoxLayout(m_suggestionSection);
    suggestions->setContentsMargins(10, 12, 22, 10);
    suggestions->setSpacing(3);
    scroll->setWidget(m_suggestionSection);

    m_recentSection = new QWidget(m_suggestionSection);
    auto* recentLayout = new QVBoxLayout(m_recentSection);
    recentLayout->setContentsMargins(0, 0, 0, 12);
    recentLayout->setSpacing(3);
    auto* recentTitle = new QLabel(tr("RECENT"), m_recentSection);
    recentLayout->addWidget(recentTitle);
    m_recentRows = new QVBoxLayout;
    m_recentRows->setSpacing(2);
    recentLayout->addLayout(m_recentRows);
    m_recentSection->hide();
    suggestions->addWidget(m_recentSection);

    auto* presetTitle = new QLabel(tr("SUGGESTIONS"), m_suggestionSection);
    presetTitle->setContentsMargins(0, 0, 0, 5);
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
        m_presetStatuses.push_back(status);
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
        const bool showExpiration = statusDuration() == QLatin1String("date_and_time");
        m_expiration->setVisible(showExpiration);
        m_expirationLabel->setVisible(showExpiration);
        updateSuggestionVisibility();
    });
    connect(m_text, &QLineEdit::textChanged, this,
            [this] { updateSuggestionVisibility(); });
    updateSuggestionVisibility();
}

QString CustomStatusEditorDialog::statusEmoji() const
{
    return m_selectedEmoji;
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
    setSelectedEmoji(status.emoji);
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
    button->setMinimumHeight(38);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(button);
    layout->setContentsMargins(8, 3, 8, 3);
    layout->setSpacing(12);

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
    label->setToolTip(status.text);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    // Keep recent and suggested statuses available when the user edits a
    // message or changes its emoji. Selecting a preset only fills the form.
    // Hiding this area caused the unexpected disappearance of both groups.

    // The web client avoids repeating a recently used status in suggestions.
    for (int i = 0; i < m_presets.size(); ++i) {
        bool duplicate = false;
        for (const auto& recent : m_recentStatuses) {
            if (m_presetStatuses.at(i).text == recent.text) {
                duplicate = true;
                break;
            }
        }
        m_presets.at(i)->setVisible(!duplicate);
    }
}


void CustomStatusEditorDialog::setSelectedEmoji(const QString& name)
{
    const QString normalized = name.trimmed();
    if (m_selectedEmoji == normalized)
        return;
    m_selectedEmoji = normalized;
    updateEmojiAction();
    updateSuggestionVisibility();
}

void CustomStatusEditorDialog::updateEmojiAction()
{
    if (!m_emojiAction)
        return;
    m_emojiAction->setIcon(statusEmojiActionIcon(
        m_registry, m_selectedEmoji, m_text->palette(), m_text->font()));
    m_emojiAction->setToolTip(m_selectedEmoji.isEmpty()
        ? tr("Choose an emoji")
        : tr("Change emoji: %1 (Backspace at the start removes it)").arg(m_selectedEmoji));
}

bool CustomStatusEditorDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_text && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Backspace && m_text->cursorPosition() == 0
            && !m_text->hasSelectedText() && !m_selectedEmoji.isEmpty()) {
            setSelectedEmoji(QString());
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void CustomStatusEditorDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
        updateEmojiAction();
}

} // namespace Mattermost
