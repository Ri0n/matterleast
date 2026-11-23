#pragma once
#include <QAbstractListModel>
#include <QMetaObject>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QVector>

namespace Mattermost {
class Backend;
class SidebarService;

// The View's single QtCore model for shared DM/GM. It stores lightweight
// account-qualified rows, never BackendChannel copies or QWidget state.
// Single-account Views keep their existing server category hierarchy.
class ViewConversationModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString sorting READ sorting NOTIFY sortingChanged)
public:
    struct Source {
        QString accountId;
        QPointer<Backend> backend;
    };
    struct Entry {
        QString accountId;
        QString channelId;
        QString title;
        quint64 activityAt = 0;
    };
    enum Role { AccountIdRole = Qt::UserRole + 1,
                ChannelIdRole, TitleRole, ActivityRole };
    Q_ENUM(Role)
    explicit ViewConversationModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString sorting() const { return _sorting; }
    bool setSorting(const QString& sorting);
    void setSources(const QVector<Source>& sources);
    Entry entryAt(int row) const;
    int rowFor(const QString& accountId, const QString& channelId) const;

public slots:
    void refresh();
signals:
    void sortingChanged();
private:
    void reconcile(const QVector<Entry>& desired);
    QVector<Source> _sources;
    QVector<Entry> _rows;
    QVector<QMetaObject::Connection> _connections;
    QHash<Backend*, QPointer<SidebarService>> _observedSidebars;
    QString _sorting = QStringLiteral("recent");
};

} // namespace Mattermost
