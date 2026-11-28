#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include "AccountConnectionState.h"

namespace Mattermost {

// Presentation projection for both Qt Widgets and a future QML view.
// No Backend, QWidget, keychain or network dependencies.
// Account identity and membership remain authoritative elsewhere.
class AccountListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString selectedAccountId READ selectedAccountId
               NOTIFY selectedAccountChanged)
public:
    struct Entry {
        QString id;
        QString displayName;
        QString server;
        AccountConnectionState state = AccountConnectionState::Disconnected;
        QString serverVersion = {};
    };

    enum Role {
        AccountIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        ServerRole,
        StatusRole,
        ConnectionStateRole,
        SelectedRole,
        ServerVersionRole
    };
    Q_ENUM(Role)

    explicit AccountListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString selectedAccountId() const { return _selectedAccountId; }
    Q_INVOKABLE int rowForId(const QString& id) const;
    Q_INVOKABLE QString idAt(int row) const;

    // Stable IDs allow incremental insert/remove/move/dataChanged updates.
    // Never reset a complete model just because one server changed status.
    void setEntries(const QVector<Entry>& entries, const QString& selectedAccountId);

signals:
    void selectedAccountChanged(const QString& accountId);

private:
    QVector<Entry> _entries;
    QString _selectedAccountId;
};

} // namespace Mattermost
