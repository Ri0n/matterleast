#pragma once

#include <QWidget>

class QLabel;
class QTabWidget;

namespace Mattermost {
class AccountManager;
class ServersSidebarPage;

// Backend-free presentation of the *same main-window Servers tab* used by
// connected workspaces. It is only visible while no account's Channels UI
// can be shown. Neither a login dialog nor a second account catalog.
class ServersWorkspacePage final : public QWidget
{
    Q_OBJECT
public:
    explicit ServersWorkspacePage(AccountManager& accounts, QWidget* parent = nullptr);
    void setError(const QString& message);

signals:
    void accountSelected(const QString& id);
    void viewSelected(const QString& id);
    void addServerRequested();
    void serverSettingsRequested(const QString& id);
    void serverDisconnectRequested(const QString& id);
    void retryRequested(const QString& id);

private:
    ServersSidebarPage* _servers = nullptr;
    QTabWidget* _tabs = nullptr;
    QLabel* _errorLabel = nullptr;
};

} // namespace Mattermost
