#pragma once

#include <QDialog>
#include <QTimer>
#include <QVector>

#include "backend/MentionGroupService.h"

class QLineEdit;
class QPushButton;
class QTableWidget;

namespace Mattermost {

class Backend;

/**
 * Server-backed user-group browser launched from the sidebar header menu.
 * Custom groups can be created/edited; directory-backed groups remain view-only.
 */
class UserGroupsDialog final : public QDialog
{
public:
    explicit UserGroupsDialog(Backend& backend, QWidget* parent = nullptr);

private:
    void scheduleSearch(const QString& text);
    void reload();
    void showGroups(QVector<MentionGroup> groups);
    void openSelectedGroup();
    void openGroup(int row);
    void createGroup();

    Backend& backend;
    MentionGroupService& groupService;
    QLineEdit* searchEdit = nullptr;
    QTableWidget* table = nullptr;
    QPushButton* openButton = nullptr;
    QTimer searchTimer;
    QString query;
    int generation = 0;
    QVector<MentionGroup> visibleGroups;
};

} // namespace Mattermost
