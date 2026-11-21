#include "AccountListModel.h"

#include <QSet>

namespace Mattermost {
namespace {

QString statusLabel(AccountConnectionState state)
{
    switch (state) {
    case AccountConnectionState::Disconnected: return QObject::tr("Disconnected");
    case AccountConnectionState::Connecting: return QObject::tr("Connecting…");
    case AccountConnectionState::Connected: return QObject::tr("Connected");
    case AccountConnectionState::Unavailable: return QObject::tr("Unavailable — retry");
    case AccountConnectionState::AuthenticationRequired:
        return QObject::tr("Sign-in required");
    case AccountConnectionState::SetupRequired: return QObject::tr("Account setup required");
    }
    return {};
}

} // namespace

AccountListModel::AccountListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int AccountListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : _entries.size();
}

QVariant AccountListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.parent().isValid()
        || index.column() != 0 || index.row() < 0
        || index.row() >= _entries.size())
        return {};

    const Entry& entry = _entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("%1\n%2 — %3")
            .arg(entry.displayName, entry.server, statusLabel(entry.state));
    case AccountIdRole: return entry.id;
    case DisplayNameRole: return entry.displayName;
    case ServerRole: return entry.server;
    case StatusRole: return statusLabel(entry.state);
    case ConnectionStateRole: return static_cast<int>(entry.state);
    case SelectedRole: return entry.id == _selectedAccountId;
    default: return {};
    }
}

QHash<int, QByteArray> AccountListModel::roleNames() const
{
    return {
        {AccountIdRole, QByteArrayLiteral("accountId")},
        {DisplayNameRole, QByteArrayLiteral("displayName")},
        {ServerRole, QByteArrayLiteral("server")},
        {StatusRole, QByteArrayLiteral("status")},
        {ConnectionStateRole, QByteArrayLiteral("connectionState")},
        {SelectedRole, QByteArrayLiteral("selected")}
    };
}

int AccountListModel::rowForId(const QString& id) const
{
    for (int row = 0; row < _entries.size(); ++row)
        if (_entries.at(row).id == id)
            return row;
    return -1;
}

QString AccountListModel::idAt(int row) const
{
    return row >= 0 && row < _entries.size()
        ? _entries.at(row).id : QString();
}

void AccountListModel::setEntries(const QVector<Entry>& entries,
                                  const QString& selectedAccountId)
{
    QSet<QString> desired;
    for (const Entry& entry : entries) {
        if (entry.id.isEmpty() || desired.contains(entry.id))
            return; // Invalid source snapshot cannot corrupt the UI projection.
        desired.insert(entry.id);
    }

    // Remove absent entries without invalidating the other rows' indexes.
    for (int row = _entries.size() - 1; row >= 0; --row) {
        if (desired.contains(_entries.at(row).id))
            continue;
        beginRemoveRows(QModelIndex(), row, row);
        _entries.removeAt(row);
        endRemoveRows();
    }

    for (int row = 0; row < entries.size(); ++row) {
        const Entry& next = entries.at(row);
        const int old = rowForId(next.id);
        if (old < 0) {
            beginInsertRows(QModelIndex(), row, row);
            _entries.insert(row, next);
            endInsertRows();
            continue;
        }
        if (old != row) {
            beginMoveRows(QModelIndex(), old, old, QModelIndex(), row);
            _entries.move(old, row);
            endMoveRows();
        }

        Entry& current = _entries[row];
        QVector<int> roles;
        if (current.displayName != next.displayName) {
            current.displayName = next.displayName;
            roles.append(DisplayNameRole);
        }
        if (current.server != next.server) {
            current.server = next.server;
            roles.append(ServerRole);
        }
        if (current.state != next.state) {
            current.state = next.state;
            roles.append(StatusRole);
            roles.append(ConnectionStateRole);
        }
        if (!roles.isEmpty()) {
            roles.append(Qt::DisplayRole);
            emit dataChanged(index(row, 0), index(row, 0), roles);
        }
    }

    if (_selectedAccountId != selectedAccountId) {
        const QString previousId = _selectedAccountId;
        _selectedAccountId = selectedAccountId;
        const int previousRow = rowForId(previousId);
        if (previousRow >= 0)
            emit dataChanged(index(previousRow, 0), index(previousRow, 0),
                             {SelectedRole});
        const int currentRow = rowForId(selectedAccountId);
        if (currentRow >= 0)
            emit dataChanged(index(currentRow, 0), index(currentRow, 0),
                             {SelectedRole});
        emit selectedAccountChanged(_selectedAccountId);
    }
}

} // namespace Mattermost
