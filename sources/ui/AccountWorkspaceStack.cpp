#include "AccountWorkspaceStack.h"

#include <QHBoxLayout>
#include <QModelIndex>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "workspaces/ViewSidebarModel.h"

namespace Mattermost {

AccountWorkspaceStack::AccountWorkspaceStack(QWidget* parent)
    : QWidget(parent)
    , _stack(new QStackedWidget(this))
{
    _stack->setObjectName(QStringLiteral("accountWorkspacePages"));
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    _mergedSidebar = new QWidget(splitter);
    _mergedSidebar->setObjectName(QStringLiteral("mergedViewSidebar"));
    _mergedSidebar->setMinimumWidth(210);
    auto* sidebarLayout = new QVBoxLayout(_mergedSidebar);
    sidebarLayout->setContentsMargins(2, 2, 2, 2);
    sidebarLayout->setSpacing(2);
    auto* servers = new QToolButton(_mergedSidebar);
    servers->setObjectName(QStringLiteral("mergedViewServersButton"));
    servers->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    servers->setToolTip(tr("Servers and views"));
    servers->setText(tr("Servers"));
    servers->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    sidebarLayout->addWidget(servers);
    connect(servers, &QToolButton::clicked, this, &AccountWorkspaceStack::showServers);

    _mergedTree = new QTreeView(_mergedSidebar);
    _mergedTree->setObjectName(QStringLiteral("mergedViewSidebarTree"));
    _mergedTree->setHeaderHidden(true);
    _mergedTree->setUniformRowHeights(true);
    _mergedTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sidebarLayout->addWidget(_mergedTree, 1);
    connect(_mergedTree, &QTreeView::clicked, this, [this](const QModelIndex& index) {
        if (!_mergedModel || !index.isValid()) return;
        const QString account = index.data(ViewSidebarModel::AccountIdRole).toString();
        if (account.isEmpty()) return;
        switch (index.data(ViewSidebarModel::KindRole).toInt()) {
        case ViewSidebarModel::Account:
            emit accountRequested(account);
            break;
        case ViewSidebarModel::Channel: {
            const QString channel = index.data(ViewSidebarModel::ChannelIdRole).toString();
            if (!channel.isEmpty()) emit conversationRequested(account, channel);
            break;
        }
        default: break;
        }
    });

    splitter->addWidget(_mergedSidebar);
    splitter->addWidget(_stack);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({280, 900});
    _mergedSidebar->hide();
    layout->addWidget(splitter);

    connect(_stack, &QStackedWidget::currentChanged, this, [this](int) {
        updateMergedSidebarVisibility();
        emit activeWorkspaceChanged(activeAccountId());
    });
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

void AccountWorkspaceStack::setMergedSidebarModel(ViewSidebarModel* model)
{
    if (_mergedModel == model) return;
    disconnect(_modelDestroyed);
    _mergedModel = model;
    _mergedTree->setModel(model);
    if (model) {
        // Expand only at View activation, not after every live row update.
        _mergedTree->expandAll();
        _modelDestroyed = connect(model, &QObject::destroyed, this, [this] {
            _mergedModel = nullptr;
            _mergedTree->setModel(nullptr);
            updateMergedSidebarVisibility();
        });
    }
    updateMergedSidebarVisibility();
}

void AccountWorkspaceStack::updateMergedSidebarVisibility()
{
    _mergedSidebar->setVisible(_mergedModel
        && _stack->currentWidget() && _stack->currentWidget() != _serversPage);
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
    // QStackedWidget::removeWidget() does not release QObject ownership.
    // Hide before reparenting to avoid flashing a top-level window.
    view->hide();
    view->setParent(nullptr);
    return view;
}

int AccountWorkspaceStack::workspaceCount() const
{
    return _workspaces.size();
}

} // namespace Mattermost
