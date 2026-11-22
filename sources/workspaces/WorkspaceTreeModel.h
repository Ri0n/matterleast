#pragma once

#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QHash>
#include <QPointer>
#include <QString>

namespace Mattermost {

class AccountListModel;
class WorkspaceLayout;

// Read-only QtCore projection of the account/profile/view topology.
// It owns only presentation nodes. WorkspaceLayout remains the single source
// of truth for memberships, and AccountListModel supplies live status roles.
// Both Widgets (QTreeView) and QML can consume the same QAbstractItemModel.
class WorkspaceTreeModel final : public QAbstractItemModel
{
    Q_OBJECT
    Q_PROPERTY(QString activeViewId READ activeViewId NOTIFY activeViewChanged)
public:
    enum NodeKind { ProfileNode, ViewNode, AccountNode, PendingNode };
    Q_ENUM(NodeKind)
    enum Role {
        NodeKindRole = Qt::UserRole + 1,
        NodeIdRole,
        ProfileIdRole,
        ViewIdRole,
        AccountIdRole,
        ConnectionStateRole,
        StatusRole,
        ActiveRole
    };
    Q_ENUM(Role)

    explicit WorkspaceTreeModel(const WorkspaceLayout& layout,
                                const AccountListModel& accounts,
                                QObject* parent = nullptr);

    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString activeViewId() const { return _activeViewId; }

    // Call only after structural topology edits. Runtime status updates and
    // view selection never reset this model.
    void refreshTopology();
    // Synchronize highlighting after WorkspaceLayout::selectView/selectAccount.
    void syncActiveView();
    // Transient unauthenticated attempts appear in the same Servers tree but
    // are never written to WorkspaceLayout or treated as verified accounts.
    void setPendingServer(const QString& id, const QString& server,
                          const QString& status);
    void removePendingServer(const QString& id);

signals:
    void activeViewChanged(const QString& id);

private:
    struct Node {
        NodeKind kind = ProfileNode;
        QString id;
        QString name;
        QString profileId;
        QString viewId;
        QString accountId;
        QString status;
        Node* parent = nullptr;
        int row = 0;
        std::vector<std::unique_ptr<Node>> children;
    };

    Node* append(Node* owner, NodeKind kind, const QString& id,
                 const QString& name, const QString& profileId,
                 const QString& viewId, const QString& accountId = {});
    QModelIndex modelIndex(const Node* node) const;
    void notifyAccountStatus(const QString& accountId);

    struct PendingServer { QString id; QString server; QString status; };
    QVector<PendingServer> _pendingServers;
    const WorkspaceLayout& _layout;
    const AccountListModel& _accounts;
    Node _root;
    QHash<QString, Node*> _views;
    QHash<QString, QVector<Node*>> _accountOccurrences;
    QString _activeViewId;
};

} // namespace Mattermost
