#include "ServersWorkspacePage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>

#include "ServersSidebarPage.h"
#include "backend/AccountManager.h"

namespace Mattermost {

ServersWorkspacePage::ServersWorkspacePage(AccountManager& accounts, QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("serversWorkspacePage"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    _tabs = new QTabWidget(this);
    _tabs->setObjectName(QStringLiteral("startupSidebarTabs"));
    _tabs->setDocumentMode(true);
    _tabs->setMinimumWidth(270);
    _tabs->setMaximumWidth(420);
    _servers = new ServersSidebarPage(accounts, _tabs);
    const int serversIndex = _tabs->addTab(
        _servers, style()->standardIcon(QStyle::SP_ComputerIcon), QString());
    _tabs->setTabToolTip(serversIndex, tr("Servers and views"));
    _tabs->setTabWhatsThis(serversIndex, tr("Manage connected servers and layouts"));

    auto* channels = new QWidget(_tabs);
    const int channelsIndex = _tabs->addTab(channels, tr("Channels"));
    _tabs->setTabEnabled(channelsIndex, false);
    _tabs->setTabToolTip(channelsIndex, tr("Connect a server to open channels"));
    layout->addWidget(_tabs);

    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(25, 20, 25, 20);
    rightLayout->addStretch();
    auto* explanation = new QLabel(tr(
        "Select a server or view on the left.\n"
        "Connected channels open here automatically."), right);
    explanation->setObjectName(QStringLiteral("serverWorkspaceInstructions"));
    explanation->setAlignment(Qt::AlignCenter);
    explanation->setWordWrap(true);
    rightLayout->addWidget(explanation);
    _errorLabel = new QLabel(right);
    _errorLabel->setObjectName(QStringLiteral("serverWorkspaceError"));
    _errorLabel->setAlignment(Qt::AlignCenter);
    _errorLabel->setWordWrap(true);
    _errorLabel->hide();
    rightLayout->addWidget(_errorLabel);
    rightLayout->addStretch();
    layout->addWidget(right, 1);

    connect(_servers, &ServersSidebarPage::accountOpenRequested,
            this, &ServersWorkspacePage::accountSelected);
    connect(_servers, &ServersSidebarPage::viewOpenRequested,
            this, &ServersWorkspacePage::viewSelected);
    connect(_servers, &ServersSidebarPage::addServerRequested,
            this, &ServersWorkspacePage::addServerRequested);
    connect(_servers, &ServersSidebarPage::signInRequested,
            this, &ServersWorkspacePage::signInRequested);
    connect(_servers, &ServersSidebarPage::retryRequested,
            this, &ServersWorkspacePage::retryRequested);
}

void ServersWorkspacePage::setError(const QString& message)
{
    _errorLabel->setText(message);
    _errorLabel->setVisible(!message.isEmpty());
}

} // namespace Mattermost
