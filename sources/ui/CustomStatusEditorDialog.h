#pragma once

#include <functional>

#include <QDialog>
#include <QPointer>
#include <QVector>

#include "backend/types/BackendUserProps.h"

class QAction;
class QComboBox;
class QEvent;
class QLabel;
class QDateTimeEdit;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QWidget;

namespace Mattermost {

class EmojiRegistry;

// A single editor for arbitrary and suggested Mattermost custom statuses.
// Suggestions populate the same form; they never bypass explicit confirmation.
class CustomStatusEditorDialog final : public QDialog
{
    Q_OBJECT
public:
    using RemoveRecent = std::function<void(
        const BackendUserCustomStatus&, std::function<void(bool)>)>;

    CustomStatusEditorDialog(EmojiRegistry& registry,
                             const BackendUserCustomStatus& current,
                             QWidget* parent = nullptr);

    QString statusEmoji() const;
    QString statusText() const;
    QString statusDuration() const;
    QString statusExpiresAt() const;

    void setRecentStatuses(const QVector<BackendUserCustomStatus>& statuses);
    void setRemoveRecentCallback(RemoveRecent callback);

private:
    void applySuggestion(const BackendUserCustomStatus& suggestion);
    QPushButton* createSuggestionButton(const BackendUserCustomStatus& suggestion,
                                        const QString& objectName, QWidget* parent);
    void updateSuggestionVisibility();
    void setSelectedEmoji(const QString& name);
    void updateEmojiAction();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

    EmojiRegistry& m_registry;
    BackendUserCustomStatus m_initial;
    QLineEdit* m_text = nullptr;
    QAction* m_emojiAction = nullptr;
    QString m_selectedEmoji;
    QComboBox* m_duration = nullptr;
    QDateTimeEdit* m_expiration = nullptr;
    QLabel* m_expirationLabel = nullptr;
    QScrollArea* m_suggestionsScroll = nullptr;
    QWidget* m_suggestionSection = nullptr;
    QWidget* m_recentSection = nullptr;
    QVBoxLayout* m_recentRows = nullptr;
    QVector<BackendUserCustomStatus> m_recentStatuses;
    QVector<QPushButton*> m_presets;
    QVector<BackendUserCustomStatus> m_presetStatuses;
    RemoveRecent m_removeRecent;
};

} // namespace Mattermost
