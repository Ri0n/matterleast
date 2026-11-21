#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>

class QStackedWidget;

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

    // Offline account chooser is a permanent page, not a temporary Backend.
    void setOverviewPage(QWidget* page);
    void showOverview();

signals:
    void activeWorkspaceChanged(const QString& accountId);

private:
    QStackedWidget* _stack;
    QWidget* _overviewPage = nullptr;
    QHash<QString, QPointer<QWidget>> _workspaces;
    QHash<QWidget*, QString> _owners;
};

} // namespace Mattermost
