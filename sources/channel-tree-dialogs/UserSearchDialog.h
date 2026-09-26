/**
 * @file UserSearchDialog.h
 * @brief Server-backed user search and DM/GM conversation picker.
 */

#pragma once

#include <functional>

#include <QMap>
#include <QSet>
#include <QTimer>
#include <QVector>

#include "UserListDialog.h"
#include "backend/UserProfileService.h"

class QHBoxLayout;
class QPushButton;
class QToolButton;
class QWidget;

namespace Mattermost {

class Backend;
class BackendChannel;

class UserSearchDialog : public UserListDialog {
public:
    enum class Mode {
        UserSelection,
        ConversationPicker,
    };

    UserSearchDialog(Backend& backend,
                     const FilterListDialogConfig& cfg,
                     UserSearchOptions options,
                     const QSet<QString>& disabledUserIds = {},
                     QWidget* parent = nullptr,
                     Mode mode = Mode::UserSelection);

    static UserSearchDialog* showConversationPicker(Backend& backend,
                                                    QWidget* parent = nullptr);

    const BackendUser* getSelectedUser();
    void setItemCountLabel(uint32_t count) override;

public slots:
    void accept() override;

private:
    void setupConversationUi();
    void filterEdited(const QString& text);
    void performSearch();
    void showUsers(const QVector<const BackendUser*>& serverUsers);
    bool matchesSearch(const BackendUser& user) const;
    bool matchesSearch(const BackendChannel& channel) const;

    void handleConversationDoubleClick(int row);
    void addParticipants(const QStringList& userIds,
                         const QString& preferredConversationId = QString());
    void removeParticipant(const QString& userId);
    void rebuildParticipantChips();
    void updateConversationActions();
    void openSelectedConversation();
    void createConversation();

    QString conversationIdForRow(int row) const;
    QStringList participantIdsForRow(int row) const;
    QStringList participantIdsForChannel(const BackendChannel& channel) const;
    bool containsSelectedParticipants(const QStringList& participantIds) const;
    bool exactlyMatchesSelectedParticipants(const QStringList& participantIds) const;
    void ensureGroupParticipants(BackendChannel& channel,
                                 std::function<void()> callback = {});

    Backend& backend;
    FilterListDialogConfig cfg;
    UserSearchOptions searchOptions;
    QSet<QString> disabledUserIds;
    QTimer searchTimer;
    QString searchTerm;
    int searchGeneration = 0;
    bool handledExistingConversation = false;
    Mode mode = Mode::UserSelection;

    QWidget* participantHost = nullptr;
    QHBoxLayout* participantLayout = nullptr;
    QPushButton* openConversationButton = nullptr;
    QPushButton* newConversationButton = nullptr;
    QSet<QString> selectedParticipantIds;
    QVector<const BackendUser*> lastServerUsers;
    QString preferredConversationId;
    QSet<QString> hydratedGroupChannels;
    QSet<QString> pendingGroupMemberLoads;
    QMap<QString, QVector<std::function<void()>>> groupMemberWaiters;
};

} // namespace Mattermost
