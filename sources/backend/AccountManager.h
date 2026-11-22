#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "workspaces/AccountListModel.h"
#include "workspaces/WorkspaceTreeModel.h"
#include "workspaces/WorkspaceLayout.h"

namespace Mattermost {
class Account;
class Backend;

// Single owner of durable account QObjects and optional session Backends.
// WorkspaceLayout remains pure serializable configuration; Widgets and QML
// consume listModel() and signals, never mutate connection maps themselves.
class AccountManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* workspaceTreeModel READ workspaceTreeModel CONSTANT)
public:
    explicit AccountManager(QObject* parent = nullptr);

    bool load(const QByteArray& json, QString* error = nullptr);
    bool loadFromOptions(QString* error = nullptr);
    bool empty() const { return _layout.accounts.isEmpty(); }
    bool invalidCatalog() const { return _invalidCatalog; }
    const WorkspaceLayout& layout() const { return _layout; }
    AccountListModel& listModel() { return _listModel; }
    const AccountListModel& listModel() const { return _listModel; }
    QAbstractItemModel* workspaceTreeModel() { return &_treeModel; }

    Account* account(const QString& id) const { return _byId.value(id, nullptr); }
    Account* accountForBackend(const Backend* backend) const;
    QVector<Account*> allAccounts() const;
    QVector<Backend*> registeredBackends() const;

    QString selectedAccountId() const { return _selectedId; }
    Backend* activeBackend() const;
    bool selectAccount(const QString& id);
    void persist() const;

    // Temporary candidates for user-entered credentials belong to this
    // manager until successfully reparented to their durable Account.
    Backend* createLoginBackend();
    void discardCandidate(Backend* candidate);
    Account* adoptAuthenticated(Backend* candidate, const QString& loginName,
                                const QString& displayName,
                                const QString& recoveryId = {},
                                bool persistToken = true);
    Backend* findAccount(const QString& server, const QString& userId) const;

    void restoreAccount(const QString& id);
    void restoreAll();
    void cancelPendingRestore(const QString& id, AccountConnectionState status);

    // UI releases its widget tree between prepareStop() and finishStop().
    // This ordering avoids dangling references to Backend-owned Storage.
    void prepareStop(Account* account, bool authenticationExpired);
    void finishStop(Account* account, bool authenticationExpired);

signals:
    void accountRegistered(Backend* backend);
    void accountUnregistered(Backend* backend);
    void activeBackendChanged(Backend* backend);
    void accountConnected(Account* account, Backend* backend);
    // Emitted only after a verified account's token is safely stored in
    // platform keychain. The migration layer may then drop old plaintext.
    void credentialsStored(const QString& server, const QString& loginName);

private:
    Account* ensureAccount(const QString& id);
    void rebuildModel();
    void finishBackendRemoval(Account* account, Backend* backend);

    WorkspaceLayout _layout;
    QHash<QString, Account*> _byId;
    AccountListModel _listModel;
    WorkspaceTreeModel _treeModel;
    QString _selectedId;
    bool _invalidCatalog = false;
};

} // namespace Mattermost
