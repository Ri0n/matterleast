#include "AccountWorkspaceStack.h"

#include <QStackedWidget>
#include <QVBoxLayout>

namespace Mattermost {

AccountWorkspaceStack::AccountWorkspaceStack(QWidget* parent)
    : QWidget(parent)
    , _stack(new QStackedWidget(this))
{
    _stack->setObjectName(QStringLiteral("accountWorkspacePages"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_stack);
    connect(_stack, &QStackedWidget::currentChanged, this,
            [this](int) { emit activeWorkspaceChanged(activeAccountId()); });
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

void AccountWorkspaceStack::setOverviewPage(QWidget* page)
{
    if (!page || _overviewPage)
        return;
    _overviewPage = page;
    _stack->insertWidget(0, page);
}

void AccountWorkspaceStack::showOverview()
{
    if (_overviewPage && _stack->currentWidget() != _overviewPage)
        _stack->setCurrentWidget(_overviewPage);
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
