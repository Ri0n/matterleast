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
            [this](int) { emit activeWorkspaceChanged(activeBackend()); });
}

bool AccountWorkspaceStack::addWorkspace(Backend& backend, QWidget* view)
{
    if (!view || _workspaces.contains(&backend) || _owners.contains(view))
        return false;

    _workspaces.insert(&backend, view);
    _owners.insert(view, &backend);
    _stack->addWidget(view);
    return true;
}

QWidget* AccountWorkspaceStack::workspace(const Backend& backend) const
{
    return _workspaces.value(&backend).data();
}

bool AccountWorkspaceStack::activateWorkspace(Backend& backend)
{
    QWidget* view = workspace(backend);
    if (!view)
        return false;
    if (_stack->currentWidget() != view)
        _stack->setCurrentWidget(view);
    return true;
}

Backend* AccountWorkspaceStack::activeBackend() const
{
    return _owners.value(_stack->currentWidget(), nullptr);
}

QWidget* AccountWorkspaceStack::takeWorkspace(Backend& backend)
{
    QWidget* view = workspace(backend);
    if (!view)
        return nullptr;

    _workspaces.remove(&backend);
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
