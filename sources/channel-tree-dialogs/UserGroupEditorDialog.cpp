#include "UserGroupEditorDialog.h"

#include <algorithm>

#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "UserGroupEditorPolicy.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendUser.h"

namespace Mattermost {
namespace {

constexpr int UserSearchDelayMs = 250;
constexpr int MinimumUserSearchLength = 2;
constexpr int UserSearchLimit = 50;

} // namespace

UserGroupEditorDialog::UserGroupEditorDialog(
    Backend& sourceBackend,
    std::optional<MentionGroup> sourceGroup,
    QWidget* parent)
    : QDialog(parent)
    , backend(sourceBackend)
    , groupService(MentionGroupService::instance(sourceBackend))
    , group(std::move(sourceGroup))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(group ? tr("Edit user group") : tr("Create user group"));
    resize(560, 620);

    buildUi();

    searchTimer.setSingleShot(true);
    searchTimer.setInterval(UserSearchDelayMs);
    connect(&searchTimer, &QTimer::timeout,
            this, &UserGroupEditorDialog::performUserSearch);

    if (group) {
        displayNameEdit->setText(group->displayName);
        mentionEdit->setText(QStringLiteral("@") + group->name);
        loadMembers();
    } else {
        rebuildMembers();
    }

    const bool canEdit = editable();
    displayNameEdit->setReadOnly(!canEdit);
    mentionEdit->setReadOnly(!canEdit);
    userSearchEdit->setEnabled(canEdit);
    userSearchResults->setEnabled(canEdit);
    addPersonButton->setEnabled(false);
    removeMemberButton->setEnabled(false);
    saveButton->setVisible(canEdit);
    if (!canEdit) {
        if (auto* buttonBox = findChild<QDialogButtonBox*>()) {
            if (QPushButton* closeButton =
                    buttonBox->button(QDialogButtonBox::Cancel)) {
                closeButton->setText(tr("Close"));
            }
        }
    }

    if (!canEdit) {
        auto* note = new QLabel(
            tr("This group is managed by the server directory and is read-only here."),
            this);
        note->setWordWrap(true);
        if (auto* layout = qobject_cast<QVBoxLayout*>(this->layout())) {
            layout->insertWidget(1, note);
        }
    }
}

void UserGroupEditorDialog::buildUi()
{
    auto* layout = new QVBoxLayout(this);

    auto* form = new QFormLayout;
    displayNameEdit = new QLineEdit(this);
    displayNameEdit->setMaxLength(64);
    displayNameEdit->setPlaceholderText(tr("Name"));
    form->addRow(tr("Name:"), displayNameEdit);

    mentionEdit = new QLineEdit(this);
    mentionEdit->setMaxLength(65);
    mentionEdit->setPlaceholderText(tr("@mention"));
    form->addRow(tr("Mention:"), mentionEdit);
    layout->addLayout(form);

    connect(displayNameEdit, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        if (!group && !mentionEditedManually) {
            const QString suggested = suggestedGroupMention(text);
            mentionEdit->setText(
                suggested.isEmpty() ? QString() : QStringLiteral("@") + suggested);
        }
    });
    connect(mentionEdit, &QLineEdit::textEdited, this,
            [this] { mentionEditedManually = true; });

    auto* membersHeader = new QHBoxLayout;
    auto* membersTitle = new QLabel(tr("Members"), this);
    QFont titleFont = membersTitle->font();
    titleFont.setBold(true);
    membersTitle->setFont(titleFont);
    memberCountLabel = new QLabel(this);
    membersHeader->addWidget(membersTitle);
    membersHeader->addStretch(1);
    membersHeader->addWidget(memberCountLabel);
    layout->addLayout(membersHeader);

    memberTable = new QTableWidget(this);
    memberTable->setColumnCount(2);
    memberTable->setHorizontalHeaderLabels({tr("Person"), tr("Mention")});
    memberTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    memberTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    memberTable->verticalHeader()->hide();
    memberTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    memberTable->setSelectionMode(QAbstractItemView::SingleSelection);
    memberTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(memberTable, 1);

    removeMemberButton = new QPushButton(tr("Remove selected"), this);
    layout->addWidget(removeMemberButton, 0, Qt::AlignRight);
    connect(removeMemberButton, &QPushButton::clicked,
            this, &UserGroupEditorDialog::removeSelectedMember);
    connect(memberTable, &QTableWidget::itemSelectionChanged, this, [this] {
        removeMemberButton->setEnabled(
            editable() && memberTable->currentRow() >= 0);
    });

    auto* addLabel = new QLabel(tr("Add people"), this);
    QFont addFont = addLabel->font();
    addFont.setBold(true);
    addLabel->setFont(addFont);
    layout->addWidget(addLabel);

    userSearchEdit = new QLineEdit(this);
    userSearchEdit->setPlaceholderText(tr("Search people"));
    layout->addWidget(userSearchEdit);

    userSearchResults = new QListWidget(this);
    userSearchResults->setMaximumHeight(150);
    userSearchResults->hide();
    layout->addWidget(userSearchResults);

    addPersonButton = new QPushButton(tr("Add selected person"), this);
    addPersonButton->setEnabled(false);
    addPersonButton->hide();
    layout->addWidget(addPersonButton, 0, Qt::AlignRight);

    const auto addSelectedSearchUser = [this] {
        QListWidgetItem* item = userSearchResults->currentItem();
        if (!item || !editable()) {
            return;
        }
        const QString userId = item->data(Qt::UserRole).toString();
        if (BackendUser* user = backend.getStorage().getUserById(userId)) {
            addUser(*user);
        }
    };

    connect(userSearchEdit, &QLineEdit::textChanged,
            this, &UserGroupEditorDialog::scheduleUserSearch);
    connect(userSearchResults, &QListWidget::itemSelectionChanged, this, [this] {
        addPersonButton->setEnabled(
            editable() && userSearchResults->currentItem());
    });
    connect(userSearchResults, &QListWidget::itemDoubleClicked,
            this, [addSelectedSearchUser](QListWidgetItem*) {
        addSelectedSearchUser();
    });
    connect(addPersonButton, &QPushButton::clicked,
            this, addSelectedSearchUser);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    saveButton = buttons->addButton(
        group ? tr("Save") : tr("Create group"),
        QDialogButtonBox::AcceptRole);
    if (!group) {
        buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    }
    layout->addWidget(buttons);

    connect(saveButton, &QPushButton::clicked,
            this, &UserGroupEditorDialog::save);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
}

bool UserGroupEditorDialog::editable() const
{
    return !group || (group->isCustom() && !group->isArchived());
}

void UserGroupEditorDialog::loadMembers()
{
    if (!group) {
        return;
    }

    memberCountLabel->setText(tr("Loading…"));
    const QString groupId = group->id;
    QPointer<UserGroupEditorDialog> guard(this);
    groupService.retrieveMembers(
        groupId,
        [guard](QVector<MentionGroupMember> loaded) mutable {
            if (!guard) {
                return;
            }

            guard->members.clear();
            guard->initialMemberIds.clear();
            for (MentionGroupMember& member : loaded) {
                if (member.id.isEmpty()) {
                    continue;
                }
                guard->initialMemberIds.insert(member.id);
                guard->members.insert(member.id, std::move(member));
            }
            guard->rebuildMembers();
        });
}

void UserGroupEditorDialog::rebuildMembers()
{
    memberTable->setRowCount(0);

    QVector<MentionGroupMember> ordered;
    ordered.reserve(members.size());
    for (auto it = members.cbegin(); it != members.cend(); ++it) {
        ordered.push_back(it.value());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const MentionGroupMember& lhs, const MentionGroupMember& rhs) {
        const QString left =
            lhs.displayName.isEmpty() ? lhs.username : lhs.displayName;
        const QString right =
            rhs.displayName.isEmpty() ? rhs.username : rhs.displayName;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });

    for (const MentionGroupMember& member : ordered) {
        const int row = memberTable->rowCount();
        memberTable->insertRow(row);

        auto* nameItem = new QTableWidgetItem(
            member.displayName.isEmpty() ? member.username : member.displayName);
        nameItem->setData(Qt::UserRole, member.id);
        memberTable->setItem(row, 0, nameItem);
        memberTable->setItem(
            row, 1,
            new QTableWidgetItem(member.username.isEmpty()
                ? QString() : QStringLiteral("@") + member.username));
    }

    memberCountLabel->setText(tr("%n member(s)", nullptr, members.size()));
    removeMemberButton->setEnabled(
        editable() && memberTable->currentRow() >= 0);
}

void UserGroupEditorDialog::scheduleUserSearch(const QString& text)
{
    searchTimer.stop();
    searchTerm = text.trimmed();
    ++searchGeneration;

    userSearchResults->clear();
    userSearchResults->hide();
    addPersonButton->setEnabled(false);
    addPersonButton->hide();

    if (!editable() || searchTerm.size() < MinimumUserSearchLength) {
        return;
    }
    searchTimer.start();
}

void UserGroupEditorDialog::performUserSearch()
{
    if (!editable() || searchTerm.size() < MinimumUserSearchLength) {
        return;
    }

    UserSearchOptions options;
    options.term = searchTerm;
    options.limit = UserSearchLimit;

    const int generation = searchGeneration;
    QPointer<UserGroupEditorDialog> guard(this);
    UserProfileService::instance(backend).searchUsers(
        options,
        [guard, generation](QVector<const BackendUser*> users) mutable {
            if (!guard || generation != guard->searchGeneration) {
                return;
            }
            guard->showUserSearchResults(users);
        });
}

void UserGroupEditorDialog::showUserSearchResults(
    const QVector<const BackendUser*>& users)
{
    userSearchResults->clear();

    for (const BackendUser* user : users) {
        if (!user || user->id.isEmpty() || members.contains(user->id)) {
            continue;
        }

        QString label = user->getDisplayName();
        if (label.isEmpty()) {
            label = user->username;
        }
        if (!user->username.isEmpty()
            && label.compare(user->username, Qt::CaseInsensitive) != 0) {
            label += QStringLiteral("  @") + user->username;
        }

        auto* item = new QListWidgetItem(label, userSearchResults);
        item->setData(Qt::UserRole, user->id);
    }

    const bool hasResults = userSearchResults->count() > 0;
    userSearchResults->setVisible(hasResults);
    addPersonButton->setVisible(hasResults);
    addPersonButton->setEnabled(
        hasResults && userSearchResults->currentItem());
}

void UserGroupEditorDialog::addUser(const BackendUser& user)
{
    if (user.id.isEmpty() || members.contains(user.id)) {
        return;
    }

    MentionGroupMember member;
    member.id = user.id;
    member.username = user.username;
    member.displayName = user.getDisplayName();
    members.insert(member.id, std::move(member));

    rebuildMembers();
    scheduleUserSearch(userSearchEdit->text());
}

void UserGroupEditorDialog::removeSelectedMember()
{
    if (!editable()) {
        return;
    }

    const int row = memberTable->currentRow();
    QTableWidgetItem* item = row >= 0 ? memberTable->item(row, 0) : nullptr;
    if (!item) {
        return;
    }

    const QString userId = item->data(Qt::UserRole).toString();
    if (!userId.isEmpty()) {
        members.remove(userId);
        rebuildMembers();
    }
}

QString UserGroupEditorDialog::validationMessage() const
{
    switch (validateUserGroupEditor(
        displayNameEdit->text(),
        mentionEdit->text(),
        members.size(),
        !group.has_value())) {
    case UserGroupValidationError::None:
        return {};
    case UserGroupValidationError::EmptyDisplayName:
        return tr("Name is required.");
    case UserGroupValidationError::EmptyMention:
        return tr("Mention is required.");
    case UserGroupValidationError::InvalidMention:
        return tr("Mention may contain only lowercase letters, numbers, '.', '-' and '_', and must be at most 64 characters.");
    case UserGroupValidationError::ReservedMention:
        return tr("This mention is reserved by Mattermost.");
    case UserGroupValidationError::MissingMembers:
        return tr("Add at least one person to the group.");
    }
    return tr("Invalid group.");
}

void UserGroupEditorDialog::save()
{
    if (!editable() || saving) {
        return;
    }

    const QString error = validationMessage();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }

    const QString displayName = displayNameEdit->text().trimmed();
    const QString mention = canonicalGroupMention(mentionEdit->text());

    saving = true;
    saveButton->setEnabled(false);

    if (!group) {
        QStringList userIds = members.keys();
        groupService.createCustomGroup(
            displayName, mention, userIds,
            [guard = QPointer<UserGroupEditorDialog>(this)](
                MentionGroupMutationResult result) mutable {
                if (guard) {
                    guard->finishMutation(result);
                }
            });
        return;
    }

    saveExisting(displayName, mention);
}

void UserGroupEditorDialog::saveExisting(const QString& displayName,
                                         const QString& mention)
{
    if (!group) {
        return;
    }

    QStringList additions;
    QStringList removals;
    QSet<QString> currentIds;
    for (auto it = members.cbegin(); it != members.cend(); ++it) {
        currentIds.insert(it.key());
    }

    for (const QString& userId : currentIds) {
        if (!initialMemberIds.contains(userId)) {
            additions.push_back(userId);
        }
    }
    for (const QString& userId : initialMemberIds) {
        if (!currentIds.contains(userId)) {
            removals.push_back(userId);
        }
    }

    QPointer<UserGroupEditorDialog> guard(this);
    groupService.updateCustomGroup(
        group->id, displayName, mention,
        [guard, additions, removals](MentionGroupMutationResult result) mutable {
            if (!guard) {
                return;
            }
            if (!result.ok) {
                guard->finishMutation(result);
                return;
            }
            guard->commitMembershipChanges(additions, removals);
        });
}

void UserGroupEditorDialog::commitMembershipChanges(
    const QStringList& additions,
    const QStringList& removals)
{
    if (!group) {
        MentionGroupMutationResult result;
        result.ok = false;
        result.errorMessage = tr("The group is no longer available.");
        finishMutation(result);
        return;
    }

    QPointer<UserGroupEditorDialog> guard(this);
    const auto removeNext =
        [guard, additions, removals](MentionGroupMutationResult result) {
        if (!guard) {
            return;
        }
        if (!result.ok) {
            guard->finishMutation(result);
            return;
        }

        // Record successful additions before the next request so retrying after
        // a later partial failure does not submit them again.
        for (const QString& userId : additions) {
            guard->initialMemberIds.insert(userId);
        }

        if (removals.isEmpty()) {
            guard->finishMutation(result);
            return;
        }

        guard->groupService.removeMembers(
            guard->group->id, removals,
            [guard, removals](MentionGroupMutationResult removeResult) {
                if (!guard) {
                    return;
                }
                if (removeResult.ok) {
                    for (const QString& userId : removals) {
                        guard->initialMemberIds.remove(userId);
                    }
                }
                guard->finishMutation(removeResult);
            });
    };

    if (additions.isEmpty()) {
        MentionGroupMutationResult result;
        result.ok = true;
        removeNext(result);
        return;
    }

    groupService.addMembers(group->id, additions, removeNext);
}

void UserGroupEditorDialog::finishMutation(
    const MentionGroupMutationResult& result)
{
    saving = false;
    if (saveButton) {
        saveButton->setEnabled(true);
    }

    if (!result.ok) {
        QString message = result.errorMessage;
        if (message.isEmpty()) {
            message = tr("Mattermost rejected the group update.");
        }
        QMessageBox::warning(this, windowTitle(), message);
        return;
    }

    QDialog::accept();
}

} // namespace Mattermost
