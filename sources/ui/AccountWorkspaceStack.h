#pragma once

#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QWidget>

class QStackedWidget;
class QTreeView;

namespace Mattermost {
class ViewSidebarModel;

// A Widgets-only presentation container. Keys are durable local account IDs,
// never Backend pointers, so network connection lifetime is irrelevant to this
// interface. A future QML view can use the same model without this widget.
class AccountWorkspaceStack final : public QWidget
{
    Q_OBJECT
public:
    explicit AccountWorkspaceStack(QWidget* parent = nullptr);

    bool addWorkspace(const QString& accountId, QWidget* workspace);
    bool activateWorkspace(const QString& accountId);
    // Detaches without deleting. Caller must dispose of the view before
    // Backend::reset()/logout() invalidates objects referenced by widgets.
    QWidget* takeWorkspace(const QString& accountId);
    QWidget* workspace(const QString& accountId) const;
    QString activeAccountId() const;
    int workspaceCount() const;

    // Backend-free Servers tab is a permanent page, not a temporary Backend.
    void setServersPage(QWidget* page);
    void showServers();
    // The optional merged View sidebar is shared across account pages.
    // A singleton View continues to render its original account sidebar.
    void setMergedSidebarModel(ViewSidebarModel* model);

signals:
    void activeWorkspaceChanged(const QString& accountId);
    void accountRequested(const QString& accountId);
    void conversationRequested(const QString& accountId, const QString& channelId);

private:
    void updateMergedSidebarVisibility();

    QStackedWidget* _stack;
    QWidget* _mergedSidebar = nullptr;
    QTreeView* _mergedTree = nullptr;
    QPointer<ViewSidebarModel> _mergedModel;
    QMetaObject::Connection _modelDestroyed;
    QWidget* _serversPage = nullptr;
    QHash<QString, QPointer<QWidget>> _workspaces;
    QHash<QWidget*, QString> _owners;
};

} // namespace Mattermost
