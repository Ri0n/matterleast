#pragma once

#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVector>

namespace Mattermost {

class Backend;
struct SidebarCategory;
class SidebarService;
class WorkspaceLayout;

// Read-only account-qualified Channels projection for Widgets and QML.
// The layout owns memberships and the Backends own the actual channel data.
// No BackendChannel, Qt Widgets or network session is owned by this model.
class ViewChannelsModel final : public QAbstractItemModel
{
    Q_OBJECT
    Q_PROPERTY(QString activeViewId READ activeViewId NOTIFY activeViewChanged)
public:
    enum NodeKind { AccountNode, TeamNode, CategoryNode, ChannelNode };
    Q_ENUM(NodeKind)
    enum Role {
        NodeKindRole = Qt::UserRole + 1,
        NodeIdRole,
        AccountIdRole,
        TeamIdRole,
        CategoryIdRole,
        ChannelIdRole,
        ConnectedRole,
        UnreadRole,
        MentionedRole,
        MutedRole,
        CollapsedRole,
        ActivityAtRole
    };
    Q_ENUM(Role)

    explicit ViewChannelsModel(const WorkspaceLayout& layout,
                               QObject* parent = nullptr);

    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString activeViewId() const { return _activeViewId; }
    // These are input updates, not connection-management operations.
    // A missing Backend keeps the durable account row visible as offline.
    void setBackend(const QString& accountId, Backend* backend);
    void syncView();
    void refreshAccount(const QString& accountId);

signals:
    void activeViewChanged(const QString& viewId);

private:
    struct Node {
        NodeKind kind = AccountNode;
        QString id;
        QString name;
        QString accountId;
        QString teamId;
        QString categoryId;
        QString channelId;
        bool connected = false;
        bool unread = false;
        bool mentioned = false;
        bool muted = false;
        bool collapsed = false;
        quint64 activityAt = 0;
        Node* owner = nullptr;
        int row = 0;
        std::vector<std::unique_ptr<Node>> children;
    };
    struct Desired {
        NodeKind kind = AccountNode;
        QString id;
        QString name;
        QString accountId;
        QString teamId;
        QString categoryId;
        QString channelId;
        bool connected = false;
        bool unread = false;
        bool mentioned = false;
        bool muted = false;
        bool collapsed = false;
        quint64 activityAt = 0;
        std::vector<Desired> children;
    };

    Desired describeAccount(const QString& accountId) const;
    Desired describeCategory(const QString& accountId,
                             const QString& teamId,
                             const SidebarCategory& category,
                             Backend& backend,
                             const SidebarService& sidebar) const;
    void reconcile(Node* owner, const std::vector<Desired>& desired);
    void updateData(Node* node, const Desired& desired);
    void refreshChannelActivity(const QString& accountId,
                                const QString& channelId);
    void updateAggregateActivity(Node* parent);
    QModelIndex modelIndex(const Node* node) const;

    const WorkspaceLayout& _layout;
    Node _root;
    QHash<QString, Node*> _accountNodes;
    QHash<QString, QPointer<Backend>> _backends;
    QHash<QString, QVector<QMetaObject::Connection>> _connections;
    QString _activeViewId;
};

} // namespace Mattermost
