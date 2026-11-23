#include "AccountWorkspaceStack.h"

#include <QHBoxLayout>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace Mattermost {

AccountWorkspaceStack::AccountWorkspaceStack(QWidget* parent)
    : QWidget(parent)
    , _stack(new QStackedWidget(this))
    , _splitter(new QSplitter(Qt::Horizontal, this))
    , _sidebarPane(new QWidget(_splitter))
{
    _stack->setObjectName(QStringLiteral("accountWorkspacePages"));
    _splitter->setObjectName(QStringLiteral("accountWorkspaceSplitter"));
    _splitter->setChildrenCollapsible(false);
    _sidebarPane->setObjectName(QStringLiteral("mergedAccountSidebarPane"));

    _sidebarLayout = new QVBoxLayout(_sidebarPane);
    _sidebarLayout->setContentsMargins(0, 0, 0, 0);
    _sidebarLayout->setSpacing(0);
    auto* toolbar = new QWidget(_sidebarPane);
    auto* bar = new QHBoxLayout(toolbar);
    bar->setContentsMargins(4, 2, 4, 2);
    auto* servers = new QToolButton(toolbar);
    servers->setObjectName(QStringLiteral("mergedViewServersButton"));
    servers->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    servers->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    servers->setText(tr("Servers"));
    servers->setToolTip(tr("Servers and views"));
    bar->addWidget(servers);
    bar->addStretch();
    _sidebarLayout->addWidget(toolbar);
    connect(servers, &QToolButton::clicked,
            this, &AccountWorkspaceStack::serversRequested);

    _splitter->addWidget(_sidebarPane);
    _splitter->addWidget(_stack);
    _splitter->setStretchFactor(0, 0);
    _splitter->setStretchFactor(1, 1);
    _sidebarPane->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_splitter);
    connect(_stack, &QStackedWidget::currentChanged, this, [this](int) {
        refreshSharedSidebarVisibility();
        emit activeWorkspaceChanged(activeAccountId());
    });
}

void AccountWorkspaceStack::setSharedSidebarWidget(QWidget* sidebar)
{
    if (_sharedSidebar == sidebar)
        return;
    if (QWidget* previous = _sharedSidebar.data()) {
        _sidebarLayout->removeWidget(previous);
        previous->hide();
        // Inactive View widgets remain retained by this shell without being
        // laid out, preserving expansion/selection through view changes.
        previous->setParent(this);
    }
    _sharedSidebar = sidebar;
    if (sidebar) {
        _sidebarLayout->addWidget(sidebar, 1);
        sidebar->show();
    }
    refreshSharedSidebarVisibility();
}

void AccountWorkspaceStack::refreshSharedSidebarVisibility()
{
    _sidebarPane->setVisible(_sharedSidebar && !activeAccountId().isEmpty());
}

bool AccountWorkspaceStack::addWorkspace(const QString& accountId, QWidget* view)
{
    if (accountId.isEmpty() || !view || _workspaces.contains(accountId)
        || _owners.contains(view))
        return false;

    _workspaces.insert(accountId, view);
    _owners.insert(view, accountId);
    _stack->addWidget(view);
    return true;
}

void AccountWorkspaceStack::setServersPage(QWidget* page)
{
    if (!page || _serversPage)
        return;
    _serversPage = page;
    _stack->insertWidget(0, page);
}

void AccountWorkspaceStack::showServers()
{
    if (_serversPage && _stack->currentWidget() != _serversPage)
        _stack->setCurrentWidget(_serversPage);
}

QWidget* AccountWorkspaceStack::workspace(const QString& accountId) const
{
    return _workspaces.value(accountId).data();
}

bool AccountWorkspaceStack::activateWorkspace(const QString& accountId)
{
    QWidget* view = workspace(accountId);
    if (!view)
        return false;
    if (_stack->currentWidget() != view)
        _stack->setCurrentWidget(view);
    return true;
}

QString AccountWorkspaceStack::activeAccountId() const
{
    return _owners.value(_stack->currentWidget());
}

QWidget* AccountWorkspaceStack::takeWorkspace(const QString& accountId)
{
    QWidget* view = workspace(accountId);
    if (!view)
        return nullptr;

    _workspaces.remove(accountId);
    _owners.remove(view);
    _stack->removeWidget(view);
    view->hide();
    view->setParent(nullptr);
    refreshSharedSidebarVisibility();
    return view;
}

int AccountWorkspaceStack::workspaceCount() const
{
    return _workspaces.size();
}

} // namespace Mattermost
