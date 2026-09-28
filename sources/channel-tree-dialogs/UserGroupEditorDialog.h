#pragma once

#include <optional>

#include <QDialog>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "backend/MentionGroupService.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

namespace Mattermost {

class Backend;
class BackendUser;

/**
 * Creates a custom Mattermost user group or edits one existing custom group.
 *
 * Member edits are staged in the dialog and committed together with the
 * name/mention changes when Save is pressed. Non-custom groups are view-only.
 */
class UserGroupEditorDialog final : public QDialog
{
public:
    explicit UserGroupEditorDialog(Backend& backend,
                                   std::optional<MentionGroup> group = std::nullopt,
                                   QWidget* parent = nullptr);

private:
    void buildUi();
    void loadMembers();
    void rebuildMembers();
    void scheduleUserSearch(const QString& text);
    void performUserSearch();
    void showUserSearchResults(const QVector<const BackendUser*>& users);
    void addUser(const BackendUser& user);
    void removeSelectedMember();
    void save();
    void saveExisting(const QString& displayName, const QString& mention);
    void commitMembershipChanges(const QStringList& additions,
                                 const QStringList& removals);
    void finishMutation(const MentionGroupMutationResult& result);
    QString validationMessage() const;
    bool editable() const;

    Backend& backend;
    MentionGroupService& groupService;
    std::optional<MentionGroup> group;

    QLineEdit* displayNameEdit = nullptr;
    QLineEdit* mentionEdit = nullptr;
    QLineEdit* userSearchEdit = nullptr;
    QListWidget* userSearchResults = nullptr;
    QTableWidget* memberTable = nullptr;
    QLabel* memberCountLabel = nullptr;
    QPushButton* removeMemberButton = nullptr;
    QPushButton* saveButton = nullptr;

    QTimer searchTimer;
    QString searchTerm;
    int searchGeneration = 0;
    bool mentionEditedManually = false;
    bool saving = false;

    QMap<QString, MentionGroupMember> members;
    QSet<QString> initialMemberIds;
};

} // namespace Mattermost
