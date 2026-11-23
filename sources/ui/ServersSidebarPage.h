#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QTreeView;
class QModelIndex;

namespace Mattermost {
class AccountManager;

// Shared-account domain model, per-Window Widgets presentation. No credentials
// or second account catalog live in this widget. QML can consume the same
// WorkspaceTreeModel and AccountManager mutation contract.
class ServersSidebarPage final : public QWidget
{
    Q_OBJECT
public:
    explicit ServersSidebarPage(AccountManager& accounts, QWidget* parent = nullptr);

signals:
    void accountOpenRequested(const QString& accountId);
    void viewOpenRequested(const QString& viewId);
    void addServerRequested();
    void serverSettingsRequested(const QString& accountId);
    void disconnectRequested(const QString& accountId);
    void retryRequested(const QString& accountId);

private:
    void updateSelection();
    void openSelected();
    void editSelectedAccount();
    void editLayout();

    AccountManager& _accounts;
    QTreeView* _tree = nullptr;
    QPushButton* _connection = nullptr;
};

} // namespace Mattermost
