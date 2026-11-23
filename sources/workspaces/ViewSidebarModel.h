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
class SidebarService;
class ViewConversationModel;

// One View's account-qualified sidebar projection, independent of Widgets.
// The single-account presentation continues to use its existing ChannelTree.
// This model only observes already-loaded backend state; constructing it never
// triggers HTTP requests or creates Backend/SidebarService instances.
class ViewSidebarModel final : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum Kind { Account, Team, Category, Channel, SharedDirectMessages };
    Q_ENUM(Kind)
    enum Role {
        KindRole = Qt::UserRole + 1,
        AccountIdRole,
        TeamIdRole,
        CategoryIdRole,
        ChannelIdRole
    };
    Q_ENUM(Role)

    struct Source {
        QString accountId;
        QString name;
        QPointer<Backend> backend;
    };

    explicit ViewSidebarModel(QObject* parent = nullptr);

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setSources(const QVector<Source>& sources, ViewConversationModel* conversations);
    QModelIndex findChannel(const QString& accountId, const QString& channelId) const;

public slots:
    void refresh();

private:
    struct Node {
        Kind kind = Account;
        QString id;
        QString title;
        QString accountId;
        QString teamId;
        QString categoryId;
        QString channelId;
        Node* parent = nullptr;
        int row = 0;
        std::vector<std::unique_ptr<Node>> children;
    };
    struct Desired {
        Kind kind = Account;
        QString id;
        QString title;
        QString accountId;
        QString teamId;
        QString categoryId;
        QString channelId;
        std::vector<Desired> children;
    };

    void reconcile(Node* parent, const std::vector<Desired>& desired);
    QModelIndex modelIndex(const Node* node) const;
    void observeSidebar(Backend& backend, SidebarService& sidebar);

    Node _root;
    QVector<Source> _sources;
    QPointer<ViewConversationModel> _conversations;
    QVector<QMetaObject::Connection> _connections;
    QHash<Backend*, QPointer<SidebarService>> _observedSidebars;
};

} // namespace Mattermost
