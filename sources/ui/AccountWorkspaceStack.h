#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>

class QStackedWidget;
class QSplitter;
class QVBoxLayout;

namespace Mattermost {

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
    // Widget-only shell slot: callers supply the existing ChannelTree, not
    // another data/model implementation. View owns retained sidebar Widgets.
    void setSharedSidebarWidget(QWidget* sidebar);
    QWidget* sharedSidebarWidget() const { return _sharedSidebar.data(); }

signals:
    void serversRequested();
    void activeWorkspaceChanged(const QString& accountId);

private:
    void refreshSharedSidebarVisibility();
    QStackedWidget* _stack;
    QSplitter* _splitter = nullptr;
    QWidget* _sidebarPane = nullptr;
    QVBoxLayout* _sidebarLayout = nullptr;
    QPointer<QWidget> _sharedSidebar;
    QWidget* _serversPage = nullptr;
    QHash<QString, QPointer<QWidget>> _workspaces;
    QHash<QWidget*, QString> _owners;
};

} // namespace Mattermost
