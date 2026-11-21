#pragma once

#include <QHash>
#include <QPointer>
#include <QWidget>

class QStackedWidget;

namespace Mattermost {

class Backend;

// In-memory presentation for concurrently connected accounts. Each Backend
// keeps a distinct view tree (including navigation tabs, history, sidebar,
// drafts and thread panes). Switching never rebuilds or serializes that tree.
//
// AccountSessionManager owns the Backends; this widget owns the visible
// workspaces. The application must take/destroy a workspace before ending
// its owning Backend session.
class AccountWorkspaceStack final : public QWidget
{
    Q_OBJECT
public:
    explicit AccountWorkspaceStack(QWidget* parent = nullptr);

    bool addWorkspace(Backend& backend, QWidget* workspace);
    bool activateWorkspace(Backend& backend);
    // Detaches without deleting. Caller must dispose of the view before
    // Backend::reset()/logout() invalidates objects referenced by widgets.
    QWidget* takeWorkspace(Backend& backend);
    QWidget* workspace(const Backend& backend) const;
    Backend* activeBackend() const;
    int workspaceCount() const;

signals:
    void activeWorkspaceChanged(Backend* backend);

private:
    QStackedWidget* _stack;
    QHash<const Backend*, QPointer<QWidget>> _workspaces;
    QHash<QWidget*, Backend*> _owners;
};

} // namespace Mattermost
